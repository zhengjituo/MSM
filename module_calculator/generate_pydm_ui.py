#!/usr/bin/env python3
"""Generate PyDM .ui file from msm_vars.json"""

import json
import os
import xml.etree.ElementTree as ET
from xml.dom import minidom

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
JSON_PATH = os.path.join(SCRIPT_DIR, "msm_vars.json")
UI_PATH = os.path.join(SCRIPT_DIR, "msm_pvs.ui")

PROTO = "ca://"

CUSTOM_WIDGETS = {
    "PyDMLineEdit": ("pydm.widgets.line_edit",     "QWidget"),
    "PyDMLabel":    ("pydm.widgets.label",          "QWidget"),
    "PyDMPushButton": ("pydm.widgets.pushbutton",    "QWidget"),
    "PyDMWaveformPlot": ("pydm.widgets.waveformplot", "QWidget"),
}


def channel(pv_name):
    return f"{PROTO}{pv_name}"


def build_ui(config):
    root = ET.Element("ui")
    root.set("version", "4.0")

    # Main window
    cls = ET.SubElement(root, "class")
    cls.text = "MSMDisplay"
    mw = ET.SubElement(root, "widget")
    mw.set("class", "QWidget")
    mw.set("name", "MSMDisplay")

    geom = ET.SubElement(mw, "property")
    geom.set("name", "geometry")
    rect = ET.SubElement(geom, "rect")
    for tag, val in [("x", 0), ("y", 0), ("width", 1000), ("height", 500)]:
        ET.SubElement(rect, tag).text = str(val)

    title = ET.SubElement(mw, "property")
    title.set("name", "windowTitle")
    ET.SubElement(title, "string").text = "MSM Control"

    hbox = ET.SubElement(mw, "layout")
    hbox.set("class", "QHBoxLayout")
    hbox.set("name", "mainLayout")

    for mod in config.get("modules", []):
        mod_name = mod["name"]
        grp = ET.SubElement(hbox, "item")
        gbox = ET.SubElement(grp, "widget")
        gbox.set("class", "QGroupBox")
        gbox.set("name", mod_name + "Group")

        title_prop = ET.SubElement(gbox, "property")
        title_prop.set("name", "title")
        ET.SubElement(title_prop, "string").text = mod_name

        glay = ET.SubElement(gbox, "layout")
        glay.set("class", "QGridLayout")
        glay.set("name", mod_name + "Layout")

        for idx, var in enumerate(mod.get("variables", [])):
            vname = var["name"]
            vtype = var.get("type", "int")
            vdir = var.get("dir", "io")
            is_array = "[]" in vtype
            is_action = vname == "action"
            pv_name = f"{config.get('prefix','MSM')}:{mod_name}:{vname}"
            ch_str = channel(pv_name)

            # Label
            lbl = ET.SubElement(glay, "item")
            lbl.set("row", str(idx))
            lbl.set("column", "0")
            lw = ET.SubElement(lbl, "widget")
            lw.set("class", "QLabel")
            lw.set("name", f"{mod_name}_{vname}_label")
            txt_prop = ET.SubElement(lw, "property")
            txt_prop.set("name", "text")
            ET.SubElement(txt_prop, "string").text = vname

            # Value widget
            val_item = ET.SubElement(glay, "item")
            val_item.set("row", str(idx))
            val_item.set("column", "1")

            if is_action and vdir == "i":
                # PushButton: writes 1
                bw = ET.SubElement(val_item, "widget")
                bw.set("class", "PyDMPushButton")
                bw.set("name", f"{mod_name}_{vname}")

                ch_prop = ET.SubElement(bw, "property")
                ch_prop.set("name", "channel")
                ch_prop.set("stdset", "0")
                ET.SubElement(ch_prop, "string").text = ch_str

                pp_prop = ET.SubElement(bw, "property")
                pp_prop.set("name", "pressValue")
                pp_prop.set("stdset", "0")
                ET.SubElement(pp_prop, "string").text = "1"

                txt2 = ET.SubElement(bw, "property")
                txt2.set("name", "text")
                ET.SubElement(txt2, "string").text = "Run"

            elif is_array:
                aw = ET.SubElement(val_item, "widget")
                aw.set("class", "PyDMWaveformPlot")
                aw.set("name", f"{mod_name}_{vname}")

                ch_prop = ET.SubElement(aw, "property")
                ch_prop.set("name", "init_y_channels")
                ch_prop.set("stdset", "0")
                ET.SubElement(ch_prop, "string").text = ch_str

            elif vdir in ("o", "io"):
                lbl_w = ET.SubElement(val_item, "widget")
                lbl_w.set("class", "PyDMLabel")
                lbl_w.set("name", f"{mod_name}_{vname}")

                ch_prop = ET.SubElement(lbl_w, "property")
                ch_prop.set("name", "channel")
                ch_prop.set("stdset", "0")
                ET.SubElement(ch_prop, "string").text = ch_str

            else:
                # dir == "i": editable line edit
                le = ET.SubElement(val_item, "widget")
                le.set("class", "PyDMLineEdit")
                le.set("name", f"{mod_name}_{vname}")

                ch_prop = ET.SubElement(le, "property")
                ch_prop.set("name", "channel")
                ch_prop.set("stdset", "0")
                ET.SubElement(ch_prop, "string").text = ch_str

    # Custom widgets
    cws = ET.SubElement(root, "customwidgets")
    for cls_name, (header, extends) in sorted(CUSTOM_WIDGETS.items()):
        cw = ET.SubElement(cws, "customwidget")
        ET.SubElement(cw, "class").text = cls_name
        ET.SubElement(cw, "header").text = header
        ET.SubElement(cw, "extends").text = extends

    return root


def prettify(elem):
    rough = ET.tostring(elem, encoding="unicode")
    dom = minidom.parseString(rough.encode("utf-8"))
    return dom.toprettyxml(indent="  ")


def build_py(config, ui_path):
    """Generate a Python wrapper that loads the .ui and wires up waveform channels."""
    py_path = ui_path.replace(".ui", ".py")
    prefix = config.get("prefix", "MSM")
    lines = [
        '"""Auto-generated PyDM display for MSM."""',
        "import os",
        "from qtpy.QtCore import QObject",
        "from pydm.display import Display",
        "",
        "",
        "class MSMDisplay(Display):",
        "    def __init__(self, parent=None, args=None, macros=None):",
        "        super().__init__(parent=parent, args=args, macros=macros)",
        "        self._wire_waveform_channels()",
        "",
        "    def ui_filename(self):",
        f"        return {repr(os.path.basename(ui_path))}",
        "",
        "    def ui_filepath(self):",
        f"        return os.path.join(os.path.dirname(__file__), {repr(os.path.basename(ui_path))})",
        "",
        "    def _wire_waveform_channels(self):",
        "        # PyDMWaveformPlot doesn't support channel from .ui, connect programmatically",
    ]
    for mod in config.get("modules", []):
        for var in mod.get("variables", []):
            if "[]" in var.get("type", ""):
                wname = f"{mod['name']}_{var['name']}"
                pv = f"ca://{prefix}:{mod['name']}:{var['name']}"
                lines.append(f"        wf = self.findChild(QObject, '{wname}')")
                lines.append(f"        if wf and wf.channel_pairs is not None:")
                lines.append(f"            wf.addChannel(y_channel='{pv}')")
    lines.append("")
    with open(py_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Generated {py_path}")


def main():
    with open(JSON_PATH) as f:
        config = json.load(f)

    root = build_ui(config)
    xml_str = prettify(root)

    with open(UI_PATH, "w") as f:
        f.write(xml_str)

    print(f"Generated {UI_PATH}")
    build_py(config, UI_PATH)


if __name__ == "__main__":
    main()
