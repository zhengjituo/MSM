#!/usr/bin/env python3
"""从 config/pv_map.json 生成 EPICS 数据库文件。

用法:
    python3 gen_db.py                      # 输出 msm_pvs.db（保留 $(P)$(I) 宏）
    python3 gen_db.py --hardcode           # 前缀固化（适合单装置部署）
    python3 gen_db.py -o /tmp/x.db

设计原则:
    - pv_map.json 是唯一真源，本脚本是派生物，不要手改输出文件。
    - rate 由 msm_epics 节拍器控制，故所有 record 均为 SCAN=Passive。
    - ADEL=0 保证每次 put 都产生 monitor 事件，PyDM 不漏更新。
"""

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_MAP = os.path.join(ROOT, "config", "pv_map.json")
DEFAULT_PREFIX_CFG = os.path.join(ROOT, "config", "pre.cfg")


def load_prefix_cfg(path):
    """读取 KEY=VALUE 形式的前缀配置。"""
    env = {}
    if not os.path.exists(path):
        return env
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("export "):
                line = line[len("export "):].strip()
            if "=" in line:
                k, v = line.split("=", 1)
                env[k.strip()] = v.strip().strip('"').strip("'")
    return env


def expand(name, prefix_env, hardcode):
    """把 ${P}${I} 宏展开（或保留）。"""
    if hardcode:
        out = name.replace("${P}", prefix_env.get("P", "")).replace(
            "${I}", prefix_env.get("I", "")
        )
    else:
        out = name
        if "${P}" not in name and "P=" in name:
            pass
    # 保留宏形式时，转成 EPICS 的 $(P)$(I)
    out = out.replace("${P}", "$(P)").replace("${I}", "$(I)")
    return out


def sanitize_record_name(pv):
    """EPICS record 名不能含空白或引号。"""
    if not re.fullmatch(r"[\w:\.\-\$\(\)\{\}]+", pv):
        sys.stderr.write(f"[warn] PV name has unusual chars: {pv}\n")
    return pv


def gen_ai(pv, egu, prec, lo=0, hi=0):
    lines = [
        f'record(ai, "{sanitize_record_name(pv)}") {{',
        '    field(DTYP, "Soft Channel")',
        f'    field(PREC, "{prec}")',
    ]
    if egu:
        lines.append(f'    field(EGU,  "{egu}")')
    if hi or lo:
        lines.append(f'    field(HIHI, "{hi}")')
        lines.append(f'    field(LOLO, "{lo}")')
        lines.append('    field(HHSV, "MAJOR")')
        lines.append('    field(HSV,  "MINOR")')
    # LOPR/HOPR（raw 钳制上下限，ai 记录没有 DRVL/DRVH）不设时默认为 0：
    # softChannel 写入的负值（frame_index=-1 自动模式、负电压 min/max/vpp、
    # 负束流位置 x/y）会被钳到 0，界面全显示 0。置 ±1e9 相当于不钳制。
    lines += [
        '    field(LOPR, "-1e9")',
        '    field(HOPR, "1e9")',
        '    field(ADEL, "0")',
        '    field(SCAN, "Passive")',
        '}',
        '',
    ]
    return "\n".join(lines)


def gen_bi(pv):
    return "\n".join([
        f'record(bi, "{pv}") {{',
        '    field(DTYP, "Soft Channel")',
        '    field(ZNAM, "Off")',
        '    field(ONAM, "On")',
        '    field(SCAN, "Passive")',
        '}',
        '',
    ])


def gen_bo(pv, znam="Idle", onam="Go"):
    """命令型 PV：写 1 触发，msm_epics 侧负责复位。

    坑（实测 EPICS 7.0.8）：显式写 `field(HIGH, "1")` 会让 bo 的 VAL 在
    约 1 秒后自己弹回 0（HIGH 本身就是 High State，默认值就是 1，显式写出
    反而触发 record 的 SPC_MOD 重算）。去掉该行后 1 能稳定保持。
    """
    return "\n".join([
        f'record(bo, "{pv}") {{',
        '    field(DTYP, "Soft Channel")',
        f'    field(ZNAM, "{znam}")',
        f'    field(ONAM, "{onam}")',
        '    field(SCAN, "Passive")',
        '}',
        '',
    ])


def gen_stringout(pv, initial=""):
    """PyDM 写入的字符串命令通道（桥订阅后转发给 IPC）。"""
    return "\n".join([
        f'record(stringout, "{pv}") {{',
        '    field(DTYP, "Soft Channel")',
        f'    field(VAL, "{initial}")',
        '    field(SCAN, "Passive")',
        '}',
        '',
    ])


def gen_longout(pv, initial=0):
    """数值命令通道（桥订阅后拼成 IPC 命令）。"""
    return "\n".join([
        f'record(longout, "{pv}") {{',
        '    field(DTYP, "Soft Channel")',
        f'    field(VAL, "{initial}")',
        '    field(SCAN, "Passive")',
        '}',
        '',
    ])


def gen_stringin(pv, initial=""):
    """桥回写的只读字符串（响应/状态文本）。"""
    return "\n".join([
        f'record(stringin, "{pv}") {{',
        '    field(DTYP, "Soft Channel")',
        f'    field(VAL, "{initial}")',
        '    field(SCAN, "Passive")',
        '}',
        '',
    ])


def gen_waveform(pv, nelm, ftype="ULONG"):
    return "\n".join([
        f'record(waveform, "{pv}") {{',
        '    field(DTYP, "Soft Channel")',
        f'    field(NELM, "{nelm}")',
        f'    field(FTVL, "{ftype}")',
        '    field(SCAN, "Passive")',
        '}',
        '',
    ])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-m", "--map", default=DEFAULT_MAP)
    ap.add_argument("-o", "--out", default=os.path.join(HERE, "msm_pvs.db"))
    ap.add_argument("--prefix-cfg", default=DEFAULT_PREFIX_CFG)
    ap.add_argument("--hardcode", action="store_true",
                    help="把 P/I 前缀固化进 PV 名（单装置部署）")
    args = ap.parse_args()

    with open(args.map, encoding="utf-8") as f:
        cfg = json.load(f)

    prefix_env = load_prefix_cfg(args.prefix_cfg)
    if args.hardcode and not prefix_env:
        sys.stderr.write(f"[warn] prefix cfg not found: {args.prefix_cfg}\n")

    chunks = [
        "# ============================================================",
        "# msm_pvs.db —— 由 gen_db.py 从 pv_map.json 生成，请勿手改",
        f"# source: {os.path.relpath(args.map, ROOT)}",
        "# 加载方式:",
        '#   softIoc -m "P=MSM:,I=" -d msm_pvs.db',
        "# ============================================================",
        "",
    ]

    seen = set()
    count = 0
    for gname, group in cfg.get("groups", {}).items():
        rate = group.get("rate_hz")
        if rate is not None:
            if not (2 <= rate <= 10):
                sys.stderr.write(
                    f"[warn] group '{gname}' rate_hz={rate} out of [2,10], clamped\n")
                rate = min(10, max(2, rate))
            chunks.append(f"# ---- group {gname} ({rate} Hz) ----")
        else:
            chunks.append(f"# ---- group {gname} ----")

        for item in group.get("vars", []):
            pv = expand(item["pv"], prefix_env, args.hardcode)
            if pv in seen:
                sys.stderr.write(f"[warn] duplicate PV: {pv}\n")
                continue
            seen.add(pv)
            kind = item.get("kind", "ai")
            if kind == "bi":
                chunks.append(gen_bi(pv))
            elif kind == "bo":
                chunks.append(gen_bo(pv, item.get("znam", "Idle"),
                                     item.get("onam", "Go")))
            else:
                chunks.append(gen_ai(pv, item.get("egu", ""),
                                     item.get("prec", 3),
                                     item.get("lo", 0),
                                     item.get("hi", 0)))
            count += 1

        # 波形通道
        nelm = group.get("wave_nelm")
        if nelm:
            for ch in group.get("channels", []):
                pv = expand(f"${{P}}${{I}}Process:wave_ch{ch}", prefix_env,
                            args.hardcode)
                if pv in seen:
                    continue
                seen.add(pv)
                chunks.append(gen_waveform(pv, nelm))
                count += 1

    # ---- 控制通道（PyDM → 桥 → IPC）----
    ctrl = cfg.get("control", {})
    if ctrl:
        chunks.append("# ---- control (PyDM -> bridge -> IPC) ----")
        for item in ctrl.get("commands", []):
            pv = expand(item["pv"], prefix_env, args.hardcode)
            if pv in seen:
                sys.stderr.write(f"[warn] duplicate PV: {pv}\n")
                continue
            seen.add(pv)
            chunks.append(gen_bo(pv, item.get("znam", "Idle"),
                                 item.get("onam", "Go")))
            count += 1

        sp = ctrl.get("save_pv")
        if sp:
            pv = expand(sp["pv"], prefix_env, args.hardcode)
            if pv not in seen:
                seen.add(pv)
                chunks.append(gen_bo(pv, "Disable", "Enable"))
                count += 1

        mp = ctrl.get("msg_pv")
        if mp:
            pv = expand(mp["pv"], prefix_env, args.hardcode)
            if pv not in seen:
                seen.add(pv)
                chunks.append(gen_stringout(pv))
                count += 1

        fp = ctrl.get("frame_index_pv")
        if fp:
            pv = expand(fp["pv"], prefix_env, args.hardcode)
            if pv not in seen:
                seen.add(pv)
                chunks.append(gen_longout(pv, initial=-1))
                count += 1

        st = ctrl.get("status", {})
        for key in ("backend_alive", "daq_running"):
            if key not in st:
                continue
            pv = expand(st[key], prefix_env, args.hardcode)
            if pv in seen:
                continue
            seen.add(pv)
            chunks.append(gen_bi(pv))
            count += 1
        if "resp" in st:
            pv = expand(st["resp"], prefix_env, args.hardcode)
            if pv not in seen:
                seen.add(pv)
                chunks.append(gen_stringin(pv))
                count += 1

    chunks.append(f"# total records: {count}")
    text = "\n".join(chunks)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(text)

    print(f"[gen_db] wrote {args.out} ({count} records)")
    if not args.hardcode:
        print('[gen_db] PV names keep $(P)$(I) macros; load with -m "P=..,I=.."')


if __name__ == "__main__":
    main()
