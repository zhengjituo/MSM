# PyDM UI Generator Design

## Goal

Generate a Qt Designer `.ui` file from `msm_vars.json` for use with PyDM, so each EPICS PV gets a visual widget without manual layout work.

## Widget Mapping

| Variable | `dir` | Widget | Read-Only? |
|---|---|---|---|
| int != "action" | i | `PyDMSpinbox` | no |
| int == "action" | i | `PyDMPushButton` (writes 1) | no |
| int | o / io | `PyDMLabel` | yes |
| double | o / io | `PyDMLabel` | yes |
| double[] | o | `PyDMWaveformPlot` | yes |

Channels use `ca://MSM:Module:var` protocol (CA, port 5064).

## Layout

- One `QGroupBox` per module, arranged horizontally in a `QHBoxLayout`
- Inside each group: a `QGridLayout` — label in column 0, widget in column 1
- `PyDMPushButton` spans both columns, centered

## Generator

A single Python script `module_calculator/generate_pydm_ui.py`:

- Reads `msm_vars.json` (same directory)
- Outputs `msm_pvs.ui`
- No external dependencies beyond stdlib (xml.etree.ElementTree)
- Use `ca://` prefix for all channels

## Usage

```bash
cd module_calculator
python3 generate_pydm_ui.py
# → creates msm_pvs.ui
pydm msm_pvs.ui
```

## Files

- `module_calculator/generate_pydm_ui.py` — generator script
- `module_calculator/msm_pvs.ui` — generated output (gitignored or committed)
