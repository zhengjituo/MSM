#!/bin/bash
# MSM EPICS 启动脚本 —— 前缀的唯一注入点
#
# 三个消费者共享 config/pre.cfg 的 P / I：
#   1. softIoc     通过 -m "P=$P,I=$I" 加载 msm_pvs.db
#   2. msm_epics   通过环境变量 MSM_P / MSM_I（也支持 --prefix-cfg）
#   3. pydm        通过 --macro "P=$P,I=$I" 解析 .ui 里的 $(P)$(I)
#
# 用法:
#   ./run_epics.sh            # 起 IOC + msm_epics
#   ./run_epics.sh --pydm     # 额外起 PyDM 界面
#
# 前提: backend (msm_cli_hwdaq / msm_cli_simsrc) 已经跑起来并暴露 IPC socket。

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
CFG="$ROOT/config/pre.cfg"

# 固定用 vendored EPICS 树（module_core/extern/epics，7.0.7 系）：
# 桥链接的是它的 libca.so.4.14.4 / libCom.so.3.23.1；用户 shell 里导出的
# EPICS_BASE（如 ~/env/base-7.0.5，libCom 是 3.19.0）版本不匹配会导致
# 桥加载失败、脚本静默退出。需要外部 base 请显式设 MSM_EPICS_BASE。
EPICS_BASE="${MSM_EPICS_BASE:-$ROOT/module_core/extern/epics}"
export EPICS_BASE
export LD_LIBRARY_PATH="$EPICS_BASE/lib/linux-x86_64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# IOC 监听地址：默认只服务本机回环。要让远端 PyDM 连，设 MSM_CAS_INTF=0.0.0.0
# （同时客户端需设 MSM_IOC_ADDR=<本机IP>）。
export EPICS_CAS_INTF_ADDR_LIST="${MSM_CAS_INTF:-127.0.0.1}"

# ── 唯一前缀注入点 ──
# shellcheck disable=SC1090
set -a; source "$CFG"; set +a
export MSM_P="$P" MSM_I="$I"
echo "[run_epics] prefix: P='$P' I='$I'"

# ── 生成 .db（pv_map.json 是唯一真源）──
python3 "$HERE/gen_db.py" -m "$ROOT/config/pv_map.json" -o "$HERE/msm_pvs.db"

# ── 单实例保护 ──
# 同前缀的多个 IOC 会同时广播，CA 客户端随机连其中一个 → 同一 PV 读出不同值。
# 桥也一样：两个桥写同一批 PV 会互相打架。所以起之前先清场。
kill_stale() {
    local name="$1" pidf="$2"
    local pids=""
    [ -f "$pidf" ] && pids="$(cat "$pidf" 2>/dev/null)"
    pids="$pids $(pgrep -x "$name" 2>/dev/null | tr '\n' ' ')"
    for p in $pids; do
        [ -n "$p" ] || continue
        kill -TERM "$p" 2>/dev/null && echo "[run_epics] 清掉旧 $name (pid=$p)"
    done
    sleep 1
    for p in $pids; do
        [ -n "$p" ] || continue
        # || true：TERM 后进程自行退出时 KILL 会失败，set -e 下不能让它杀脚本
        kill -KILL "$p" 2>/dev/null || true
    done
    rm -f "$pidf"
}

kill_stale msm_epics /tmp/msm_epics.pid

# 若 IOC 由外部独立管理（例如现场已有 st.cmd），设 MSM_NO_IOC=1 跳过
if [ "${MSM_NO_IOC:-0}" = "1" ]; then
    echo "[run_epics] MSM_NO_IOC=1，复用已有 IOC"
else
    kill_stale softIoc /tmp/msm_ioc.pid

    # ── 1. 外部 IOC ──
    "$EPICS_BASE/bin/linux-x86_64/softIoc" -S \
        -m "P=$P,I=$I" -d "$HERE/msm_pvs.db" < /dev/zero > /tmp/msm_ioc.log 2>&1 &
    IOC_PID=$!
    echo "$IOC_PID" > /tmp/msm_ioc.pid
    echo "[run_epics] IOC pid=$IOC_PID  log=/tmp/msm_ioc.log"
    sleep 3
fi

# ── 2. IPC → CA 桥 ──
# 动态库完整性预检：EPICS 树不全/路径错时明确报错，而不是 loader 一句话
# 后桥立刻退出、脚本 wait 完跟着退（表现为"run_epics.sh 自己退了"）
missing="$(ldd "$HERE/build/msm_epics" 2>/dev/null | grep 'not found' || true)"
if [ -n "$missing" ]; then
    echo "[run_epics] ERROR: msm_epics 缺动态库（EPICS 树不完整或路径不对）:" >&2
    echo "$missing" >&2
    exit 1
fi

"$HERE/build/msm_epics" \
    --map "$ROOT/config/pv_map.json" \
    --prefix-cfg "$CFG" \
    --ipc "$(python3 -c "import json;print(json.load(open('$ROOT/config/pv_map.json'))['ipc'])" )" \
    > /tmp/msm_epics.log 2>&1 &
EPICS_PID=$!
echo "$EPICS_PID" > /tmp/msm_epics.pid
echo "[run_epics] msm_epics pid=$EPICS_PID  log=/tmp/msm_epics.log"
sleep 2

# ── 3. 可选 PyDM ──
if [ "$1" = "--pydm" ]; then
    python3 "$HERE/gen_pydm.py" -m "$ROOT/config/pv_map.json" -o "$HERE/msm_daq.ui"
    echo "[run_epics] starting pydm (macro P=$P I=$I)"
    pydm --macro "P=$P,I=$I" "$HERE/msm_daq.ui" &
    PYDM_PID=$!
    echo "[run_epics] pydm pid=$PYDM_PID"
fi

echo
echo "[run_epics] ready. verify:"
echo "  $EPICS_BASE/bin/linux-x86_64/caget ${P}${I}Process:ch1_frames"

wait $EPICS_PID || rc=$?
rc=${rc:-0}
if [ "$rc" != "0" ]; then
    echo "[run_epics] 桥 msm_epics 异常退出 (code $rc) — 看 /tmp/msm_epics.log" >&2
fi
