#!/usr/bin/env python3
import json, os
import xml.etree.ElementTree as ET
from xml.dom import minidom

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
JSON_PATH = os.path.join(SCRIPT_DIR, "pv_config.json")
UI_PATH = os.path.join(SCRIPT_DIR, "msm_daq.ui")
PROTO = "ca://"

CUSTOM_WIDGETS = {
    "PyDMLineEdit": ("pydm.widgets.line_edit", "QWidget"),
    "PyDMLabel":    ("pydm.widgets.label",     "QWidget"),
    "PyDMByteIndicator": ("pydm.widgets.byte", "QWidget"),
}

def E(parent, tag, text=None, **attrs):
    el = ET.SubElement(parent, tag, attrs)
    if text is not None:
        el.text = text
    return el

def P(parent, name, **extra):
    return E(parent, "property", name=name, **extra)

def build_ui(config):
    root = ET.Element("ui", attrib={"version": "4.0"})
    E(root, "class", "DAQDisplay")
    mw = E(root, "widget", **{"class": "QWidget", "name": "DAQDisplay"})

    geom = P(mw, "geometry")
    rect = E(geom, "rect")
    for t, v in [("x",0),("y",0),("width",1100),("height",650)]:
        E(rect, t, str(v))

    E(P(mw, "windowTitle"), "string", "MSM DAQ Control")
    hbox = E(mw, "layout", **{"class": "QHBoxLayout", "name": "mainLayout"})

    for mod in config.get("modules", []):
        mn = mod["name"]
        item = E(hbox, "item")
        gb = E(item, "widget", **{"class": "QGroupBox", "name": mn + "Group"})
        E(P(gb, "title"), "string", mn)

        gl = E(gb, "layout", **{"class": "QGridLayout", "name": mn + "Layout"})

        row = 0
        for var in mod.get("variables", []):
            vn = var["name"]
            vd = var.get("dir", "io")
            pv = f"{config.get('prefix','MSM')}:{mn}:{vn}"
            ch = PROTO + pv

            E(gl, "item", row=str(row), column="0")
            lb = E(gl.findall("item")[-1], "widget", **{"class": "QLabel", "name": f"{mn}_{vn}_label"})
            E(P(lb, "text"), "string", vn)

            vi = E(gl, "item", row=str(row), column="1")

            if vn in ("running", "recording"):
                w = E(vi, "widget", **{"class": "PyDMByteIndicator", "name": f"{mn}_{vn}"})
                E(P(w, "channel", stdset="0"), "string", ch)
            elif vd in ("o",):
                w = E(vi, "widget", **{"class": "PyDMLabel", "name": f"{mn}_{vn}"})
                E(P(w, "channel", stdset="0"), "string", ch)
            else:
                w = E(vi, "widget", **{"class": "PyDMLineEdit", "name": f"{mn}_{vn}"})
                E(P(w, "channel", stdset="0"), "string", ch)
            row += 1

    cws = E(root, "customwidgets")
    for cls, (hdr, ext) in sorted(CUSTOM_WIDGETS.items()):
        cw = E(cws, "customwidget")
        E(cw, "class", cls)
        E(cw, "header", hdr)
        E(cw, "extends", ext)
    return root

def prettify(elem):
    return minidom.parseString(ET.tostring(elem, encoding="unicode").encode()).toprettyxml(indent="  ")

def main():
    with open(JSON_PATH) as f:
        config = json.load(f)
    xml_str = prettify(build_ui(config))
    with open(UI_PATH, "w") as f:
        f.write(xml_str)
    print(f"Generated {UI_PATH}  ({len(config.get('modules',[]))} modules)")

if __name__ == "__main__":
    main()
