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
    python3 gen_pydm.py                    # 4 通道 / 每卡 4 通道 → 1 个 tab
    python3 gen_pydm.py --nchannels 8      # 8 通道 → 2 个 tab
    python3 gen_pydm.py --ch-per-card 2    # 每卡 2 通道
"""

import argparse
import json
import os
import xml.etree.ElementTree as ET
from xml.dom import minidom

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_MAP = os.path.join(ROOT, "config", "pv_map.json")

FEATURES = ["trig_id", "bunch_id", "min", "max", "sum_l", "sum_h", "vpp"]


# ───────────────────────── XML 小工具 ─────────────────────────
def prop(widget, name, tag, value):
    p = ET.SubElement(widget, "property", {"name": name})
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

    # 列顺序对齐 webui：Ch | DAQ | Process | Batches | Storage
    for c, h in enumerate(["CH", "DAQ", "Process", "Batches", "Storage"]):
        add_to_grid(fg, 0, 0 + c, "QLabel", f"hdr_frame_{card_no}_{c}",
                    text=("string", h))
    for r, ch in enumerate(channels, start=1):
        add_to_grid(fg, r, 0, "QLabel", f"lbl_frame_ch{ch}", text=("string", f"CH{ch}"))
        for c, (label, pattern) in enumerate(
                [("daq", "DAQ:ch%d_frames"),
                 ("proc", "Process:ch%d_frames"),
                 ("batches", "Process:ch%d_batches"),
                 ("stor", "Storage:ch%d_consumed_frames")], start=1):
            addr = pv_or_none(varmap, pattern % ch)
            name = f"lbl_{label}_ch{ch}"
            if addr:
                add_to_grid(fg, r, c, "PyDMLabel", name, channel=addr)
            else:
                add_to_grid(fg, r, c, "QLabel", name, text=("string", "-"))

    # ── 特征表（含 Index/Set 行，对齐 webui 的 batch #N idx=M + Set）──
    ft_item = ET.SubElement(vbox, "item")
    ft = ET.SubElement(ft_item, "widget",
                       {"class": "QWidget", "name": f"featPanel{card_no}"})
    ftv = ET.SubElement(ft, "layout",
                        {"class": "QVBoxLayout", "name": f"featOuter{card_no}"})

    idx_row = ET.SubElement(ftv, "item")
    idx_w = ET.SubElement(idx_row, "widget",
                          {"class": "QWidget", "name": f"idxRow{card_no}"})
    idx_h = ET.SubElement(idx_w, "layout",
                          {"class": "QHBoxLayout", "name": f"idxLayout{card_no}"})
    # 右侧对齐：占位 spacer 在前
    sp_item = ET.SubElement(idx_h, "item")
    sp = ET.SubElement(sp_item, "spacer", {"name": f"idxPush{card_no}"})
    prop(sp, "orientation", "enum", "Qt::Horizontal")
    usp = ET.SubElement(sp, "property", {"name": "sizeHint"})
    sz = ET.SubElement(usp, "size")
    ET.SubElement(sz, "width").text = "40"
    ET.SubElement(sz, "height").text = "20"
    add_to_layout(idx_h, "QLabel", f"lblIdxName{card_no}", text=("string", "Index:"))
    ed = add_to_layout(idx_h, "QLineEdit", "editFrameIndex")
    prop(ed, "maximumWidth", "number", 90)
    btn = add_to_layout(idx_h, "QPushButton", "btnSetFrameIndex",
                        text=("string", "Set"))
    prop(btn, "maximumWidth", "number", 60)

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
    ap.add_argument("--nchannels", type=int, default=4)
    ap.add_argument("--ch-per-card", type=int, default=4,
                    help="每个 tab 容纳多少通道（默认 4，即一卡 4 通道）")
    args = ap.parse_args()

    with open(args.map, encoding="utf-8") as f:
        cfg = json.load(f)

    if args.ch_per_card < 1:
        raise SystemExit("--ch-per-card 必须 >= 1")

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
