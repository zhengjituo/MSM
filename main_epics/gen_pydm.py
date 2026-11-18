#!/usr/bin/env python3
"""从 config/pv_map.json 生成 PyDM 界面（.ui）。

布局对齐 main_webui/web/msm_daq_hw.html：
    - 顶部全局状态：batch 号、frame index
    - 每个卡一个 tab（QTabWidget）
        · 帧数表：CH | DAQ | Process | Storage(consumed)（仅本卡通道）
        · 特征表：trig_id / bunch_id / min / max / sum_l / sum_h / vpp
        · 波形区：2×2 网格（与 webui 的 grid-template-columns:1fr 1fr 一致）

设计原则同 gen_db.py：pv_map.json 是唯一真源，本脚本只是派生物。

踩过的坑（都记在这，别重复踩）：
    1. 宏必须用 JSON 传（KEY=value 形式下末尾空值会被 pydm 丢弃），见 run_pydm.sh
    2. 波形图绑通道用 `curves` 属性（QStringList，每项一条 JSON），不是 channel/title
    3. QGridLayout 的 <item row col> 里要直接放 <widget>，多套一层 <item> 会让行列号失效
    4. geometry 必须用 <rect>，不能拿 <size> 顶替

用法:
    python3 gen_pydm.py                    # 通道数从 pv_map.json 自动推导
    python3 gen_pydm.py --nchannels 8      # 手动指定总通道数（覆盖推导）
    python3 gen_pydm.py --ch-per-card 2    # 每卡 2 通道
"""

import argparse
import json
import os
import re
import xml.etree.ElementTree as ET
from xml.dom import minidom

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_MAP = os.path.join(ROOT, "config", "pv_map.json")

FEATURES = ["trig_id", "bunch_id", "min", "max", "sum_l", "sum_h", "vpp"]


# ───────────────────────── XML 小工具 ─────────────────────────
def prop(widget, name, tag, value):
    # PyQt5 uic 对自定义控件的非标属性生成 set<Name>() 调用（PyDMLabel 无
    # setChannel → AttributeError）；stdset="0" 走 setProperty，pydm 的
    # channel 是 @Property(str) 真 Q_PROPERTY，setProperty 会触发 Python setter。
    # （PySide2 uic 对未知属性本就生成 setProperty，两种绑定下都成立。）
    # 例外：PyDMWaveformPlot.curves 有真实的 setCurves（官方为 .ui 加载设计），
    # 必须走默认路径，不能加 stdset="0"。
    attrs = {"name": name}
    if name == "channel":
        attrs["stdset"] = "0"
    p = ET.SubElement(widget, "property", attrs)
    ET.SubElement(p, tag).text = str(value)
    return p


def prop_size(widget, name, w, h):
    p = ET.SubElement(widget, "property", {"name": name})
    s = ET.SubElement(p, "size")
    ET.SubElement(s, "width").text = str(w)
    ET.SubElement(s, "height").text = str(h)
    return p


def prop_rect(widget, name, x, y, w, h):
    p = ET.SubElement(widget, "property", {"name": name})
    r = ET.SubElement(p, "rect")
    for tag, val in (("x", x), ("y", y), ("width", w), ("height", h)):
        ET.SubElement(r, tag).text = str(val)
    return p


def prop_stringlist(widget, name, values):
    p = ET.SubElement(widget, "property", {"name": name})
    sl = ET.SubElement(p, "stringlist")
    for v in values:
        ET.SubElement(sl, "string").text = v
    return p


def make_widget(cls, name, **props):
    w = ET.Element("widget", {"class": cls, "name": name})
    for k, (tag, v) in props.items():
        prop(w, k, tag, v)
    return w


def add_to_layout(layout, cls, name, **props):
    item = ET.SubElement(layout, "item")
    w = make_widget(cls, name, **props)
    item.append(w)
    return w


def add_to_grid(grid, row, col, cls, name, **props):
    """<item row col> 里直接放 <widget>，不能多套一层 <item>。"""
    item = ET.SubElement(grid, "item", {"row": str(row), "column": str(col)})
    w = make_widget(cls, name, **props)
    item.append(w)
    return w


def pv_or_none(varmap, key):
    pv = varmap.get(key)
    return ("cstring", f"ca://{pv}") if pv else None


# ───────────────────────── 单卡页面 ─────────────────────────
def build_card_page(tabs, card_no, channels, varmap, wave_pattern):
    page = ET.SubElement(tabs, "widget",
                          {"class": "QWidget", "name": f"tabCard{card_no}"})
    attr = ET.SubElement(page, "attribute", {"name": "title"})
    if len(channels) == 1:
        title = f"Card {card_no} (CH{channels[0]})"
    else:
        title = f"Card {card_no} (CH{channels[0]}\u2013CH{channels[-1]})"
    ET.SubElement(attr, "string").text = title

    vbox = ET.SubElement(page, "layout",
                         {"class": "QVBoxLayout", "name": f"cardLayout{card_no}"})

    # ── 帧数表 ──
    fr_item = ET.SubElement(vbox, "item")
    fr = ET.SubElement(fr_item, "widget",
                       {"class": "QWidget", "name": f"framePanel{card_no}"})
    fg = ET.SubElement(fr, "layout",
                       {"class": "QGridLayout", "name": f"frameGrid{card_no}"})

    # 列顺序对齐 webui：Ch | DAQ | Process | Batches | Consumed | 丢帧
    # （Consumed = 实际过 storage 阶段的帧数：仅保存启用时计数。
    #   不存时 process 不喂 ring2（a707931 省无效拷贝），Consumed=0 是预期，
    #   process 阶段消费看 Process 列；显示名不用模块名 Storage，
    #   避免误解为"已存盘帧数"；丢帧 = ring 溢出丢批次数，>0 即有数据丢失）
    consumed_tip = ("实际过 storage 阶段的帧数：仅保存启用时计数。"
                    "不存时 =0（process 不喂数据到 storage 阶段，省无效拷贝），"
                    "process 阶段消费看 Process 列")
    for c, h in enumerate(["CH", "DAQ", "Process", "Batches", "Consumed", "丢帧"]):
        w = add_to_grid(fg, 0, 0 + c, "QLabel", f"hdr_frame_{card_no}_{c}",
                        text=("string", h))
        if h == "Consumed":
            prop(w, "toolTip", "string", consumed_tip)
    for r, ch in enumerate(channels, start=1):
        add_to_grid(fg, r, 0, "QLabel", f"lbl_frame_ch{ch}", text=("string", f"CH{ch}"))
        for c, (label, pattern) in enumerate(
                [("daq", "DAQ:ch%d_frames"),
                 ("proc", "Process:ch%d_frames"),
                 ("batches", "Process:ch%d_batches"),
                 ("stor", "Storage:ch%d_consumed_frames"),
                 ("ovf", "DAQ:ch%d_overflow")], start=1):
            addr = pv_or_none(varmap, pattern % ch)
            name = f"lbl_{label}_ch{ch}"
            if addr:
                add_to_grid(fg, r, c, "PyDMLabel", name, channel=addr)
            else:
                add_to_grid(fg, r, c, "QLabel", name, text=("string", "-"))

    # ── 特征表 ──
    # Index/Set 不放在卡内：frame_index 是全局变量（batch 相位对齐对所有卡
    # 同义），webui 也只有一个全局入口；放每卡里会出现多个同名控件且只连
    # 上第一个。挪到顶部状态栏（build() 里），与 webui 的 batch #N idx=M 一致。
    ft_item = ET.SubElement(vbox, "item")
    ft = ET.SubElement(ft_item, "widget",
                       {"class": "QWidget", "name": f"featPanel{card_no}"})
    ftv = ET.SubElement(ft, "layout",
                        {"class": "QVBoxLayout", "name": f"featOuter{card_no}"})

    grid_item = ET.SubElement(ftv, "item")
    tg = ET.SubElement(grid_item, "layout",
                       {"class": "QGridLayout", "name": f"featGrid{card_no}"})

    add_to_grid(tg, 0, 0, "QLabel", f"hdr_feat_name_{card_no}",
                text=("string", "field"))
    for c, ch in enumerate(channels, start=1):
        add_to_grid(tg, 0, c, "QLabel", f"hdr_feat_ch{ch}", text=("string", f"CH{ch}"))
    for r, key in enumerate(FEATURES, start=1):
        add_to_grid(tg, r, 0, "QLabel", f"lbl_name_{key}_{card_no}",
                    text=("string", key))
        for c, ch in enumerate(channels, start=1):
            addr = pv_or_none(varmap, f"Process:ch{ch}_{key}")
            name = f"lbl_ch{ch}_{key}"
            if addr:
                add_to_grid(tg, r, c, "PyDMLabel", name, channel=addr)
            else:
                add_to_grid(tg, r, c, "QLabel", name, text=("string", "-"))

    # ── 波形区：2×2（对齐 webui 的 1fr 1fr）──
    wv_item = ET.SubElement(vbox, "item")
    wv = ET.SubElement(wv_item, "widget",
                       {"class": "QWidget", "name": f"wavePanel{card_no}"})
    wg = ET.SubElement(wv, "layout",
                       {"class": "QGridLayout", "name": f"waveGrid{card_no}"})

    for i, ch in enumerate(channels):
        row, col = divmod(i, 2)
        item = ET.SubElement(wg, "item", {"row": str(row), "column": str(col)})
        plot = make_widget("PyDMWaveformPlot", f"waveView_ch{ch}")
        prop(plot, "title", "string", f"Waveform CH{ch}")
        prop_size(plot, "minimumSize", 0, 190)
        curve = json.dumps({
            "y_channel": "ca://" + wave_pattern.replace("{ch}", str(ch)),
            "x_channel": None,
            "name": f"CH{ch}",
            "plot_style": "Line",
        })
        prop_stringlist(plot, "curves", [curve])
        item.append(plot)


# ───────────────────────── 构建 ─────────────────────────
def derive_nchannels(cfg):
    """从 pv_map 推导总通道数（Process:chN_frames 与 wave channels 的最大值）。

    多卡时（如 2×4=8）不必再手改 --nchannels：pv_map 加到 ch8，
    这里自动推出 2 个 tab。
    """
    nch = 0
    for g in cfg.get("groups", {}).values():
        for it in g.get("vars", []):
            m = re.match(r"Process:ch(\d+)_frames$", it.get("from", ""))
            if m:
                nch = max(nch, int(m.group(1)))
        for ch in g.get("channels", []):
            nch = max(nch, int(ch))
    return nch


def build(cfg, nch, ch_per_card=4):
    varmap = {}
    for g in cfg.get("groups", {}).values():
        for it in g.get("vars", []):
            varmap[it["from"]] = it["pv"]
    wave_pattern = cfg.get("groups", {}).get("wave", {}).get(
        "pv_pattern", "${P}${I}Process:wave_ch{ch}")

    ui = ET.Element("ui", version="4.0")
    ET.SubElement(ui, "class").text = "MSMDaqDisplay"
    top = ET.SubElement(ui, "widget",
                        {"class": "QWidget", "name": "MSMDaqDisplay"})
    prop_rect(top, "geometry", 0, 0, 1100, 900)
    prop(top, "windowTitle", "string", "MSM DAQ")

    vbox = ET.SubElement(top, "layout",
                         {"class": "QVBoxLayout", "name": "rootLayout"})

    # ── 全局状态行 ──
    sb_item = ET.SubElement(vbox, "item")
    sb = ET.SubElement(sb_item, "widget", {"class": "QWidget", "name": "statusBar"})
    hbl = ET.SubElement(sb, "layout",
                        {"class": "QHBoxLayout", "name": "statusLayout"})
    add_to_layout(hbl, "QLabel", "lblBatchName", text=("string", "  Batch:  "))
    if pv_or_none(varmap, "Process:batch_no"):
        add_to_layout(hbl, "PyDMLabel", "lblBatch",
                      channel=pv_or_none(varmap, "Process:batch_no"))
    add_to_layout(hbl, "QLabel", "lblFrameName", text=("string", "  Frame:  "))
    if pv_or_none(varmap, "Process:frame_index"):
        add_to_layout(hbl, "PyDMLabel", "lblFrame",
                      channel=pv_or_none(varmap, "Process:frame_index"))
    # Index + Set（全局，对齐 webui 的 "batch #N idx=M + Set"）：
    # frame_index 对所有卡同义（batch 相位对齐），只放一个入口。
    # -1 = 自动对齐 100ms 边界；≥0 = 固定选 batch 内第 N 帧
    add_to_layout(hbl, "QLabel", "lblIdxName", text=("string", "  Index:"))
    ed = add_to_layout(hbl, "QLineEdit", "editFrameIndex")
    prop(ed, "maximumWidth", "number", 90)
    btn = add_to_layout(hbl, "QPushButton", "btnSetFrameIndex",
                        text=("string", "Set"))
    prop(btn, "maximumWidth", "number", 60)
    add_to_layout(hbl, "QLabel", "spacer", text=("string", ""))

    # ── 多卡 tab ──
    tab_item = ET.SubElement(vbox, "item")
    tabs = ET.SubElement(tab_item, "widget",
                         {"class": "QTabWidget", "name": "cardTabs"})
    prop(tabs, "currentIndex", "number", 0)

    for i in range(0, nch, ch_per_card):
        channels = list(range(i + 1, min(i + ch_per_card, nch) + 1))
        build_card_page(tabs, i // ch_per_card + 1, channels, varmap, wave_pattern)

    # ── customwidgets ──
    cws = ET.SubElement(ui, "customwidgets")
    for cls, ext, header in (
        ("PyDMLabel", "QLabel", "pydm.widgets.label"),
        ("PyDMWaveformPlot", "QWidget", "pydm.widgets.waveformplot"),
    ):
        cw = ET.SubElement(cws, "customwidget")
        ET.SubElement(cw, "class").text = cls
        ET.SubElement(cw, "extends").text = ext
        ET.SubElement(cw, "header").text = header

    ET.SubElement(ui, "resources")
    ET.SubElement(ui, "connections")
    return ui


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-m", "--map", default=DEFAULT_MAP)
    ap.add_argument("-o", "--out", default=os.path.join(HERE, "msm_daq.ui"))
    ap.add_argument("--nchannels", type=int, default=None,
                    help="总通道数；缺省从 pv_map.json 自动推导")
    ap.add_argument("--ch-per-card", type=int, default=4,
                    help="每个 tab 容纳多少通道（默认 4，即一卡 4 通道）")
    args = ap.parse_args()

    with open(args.map, encoding="utf-8") as f:
        cfg = json.load(f)

    if args.ch_per_card < 1:
        raise SystemExit("--ch-per-card 必须 >= 1")

    nch = args.nchannels if args.nchannels is not None else derive_nchannels(cfg)
    if nch < 1:
        raise SystemExit("pv_map.json 推不出通道数，请用 --nchannels 指定")
    args.nchannels = nch

    ui = build(cfg, args.nchannels, args.ch_per_card)

    raw = ET.tostring(ui, encoding="utf-8")
    pretty = minidom.parseString(raw).toprettyxml(indent="  ", encoding="utf-8")
    with open(args.out, "wb") as f:
        f.write(pretty)
    ntab = (args.nchannels + args.ch_per_card - 1) // args.ch_per_card
    print(f"[gen_pydm] wrote {args.out} "
          f"({args.nchannels} ch, {ntab} tab(s), {args.ch_per_card} ch/card)")


if __name__ == "__main__":
    main()
