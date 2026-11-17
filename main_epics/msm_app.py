#!/usr/bin/env python3
"""MSM PyDM 主程序 —— 顶部控制条 + PyDM 显示区，操作逻辑对齐 main_webui。

链路（只有桥连 IPC，本程序是纯 CA 客户端）:

    backend ──IPC── msm_epics(桥) ──CA── 本程序

- 本程序不连 /tmp/msm_daq.sock：IPCServer 是单连接，桥常驻占用，第二个
  客户端连不进去。控制命令因此走 PV，由桥转发（见 pv_map.json 的 control）。
- config 编辑是纯文件操作（app_pending_config.json），由 msm_cli_config 完成，
  不需要 backend，也不需要 IPC。
- backend 由本程序 fork（和 webui 的 ▶ Run 一样），■ Quit 时回收。

用法:
    ./run_pydm.sh                 # 正常启动
    ./run_pydm.sh --regen         # 先重新生成 .ui
"""

import json
import os
import subprocess
import sys
import time

import numpy as _np
if not hasattr(_np, "float"):
    # pydm 1.13.0 的 waveformplot/scatterplot/timeplot 用 np.float —— 该别名
    # 在 numpy>=1.24 被移除（本机 2.2.6），导致**每次波形重绘都抛
    # AttributeError、曲线永远画不出来**。np.float 本来就是内建 float 的
    # 别名，补回它语义完全一致（只影响本进程）。
    _np.float = float

from PyQt5.QtCore import Qt, QTimer
from PyQt5.QtWidgets import (
    QApplication, QCheckBox, QComboBox, QDialog, QDialogButtonBox, QFrame,
    QGridLayout, QGroupBox, QHBoxLayout, QLabel, QLineEdit, QMainWindow,
    QMessageBox, QPushButton, QRadioButton, QTableWidget, QTableWidgetItem,
    QVBoxLayout, QWidget, QHeaderView,
)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

UI_FILE = os.path.join(HERE, "msm_daq.ui")
PV_MAP = os.path.join(ROOT, "config", "pv_map.json")
PREFIX_CFG = os.path.join(ROOT, "config", "pre.cfg")
CONFIG_FILE = os.environ.get(
    "MSM_CONFIG_FILE", os.path.join(ROOT, "config", "app_pending_config.json"))

CLI_DIR = os.environ.get("MSM_CLI_PATH", os.path.join(ROOT, "main_cli", "build"))
CFG_CLI = os.path.join(CLI_DIR, "msm_cli_config")

GREEN = "#3ec46d"
YELLOW = "#e0b13a"
RED = "#e04a4a"
GREY = "#707080"


# ───────────────────────── 配置读取 ─────────────────────────
def load_prefix():
    env = {}
    try:
        with open(PREFIX_CFG, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if line.startswith("export "):
                    line = line[7:]
                if "=" in line:
                    k, v = line.split("=", 1)
                    env[k.strip()] = v.strip().strip('"').strip("'")
    except OSError:
        pass
    env["P"] = os.environ.get("MSM_P", env.get("P", "MSM:"))
    env["I"] = os.environ.get("MSM_I", env.get("I", ""))
    return env


def expand(pv, env):
    return pv.replace("${P}", env.get("P", "")).replace("${I}", env.get("I", ""))


def load_pv_map():
    with open(PV_MAP, encoding="utf-8") as f:
        return json.load(f)


# ───────────────────────── PV 访问 ─────────────────────────
class Pvs:
    """pyepics 薄封装：monitor 缓存 + 非阻塞写。

    GUI 线程里绝不做阻塞 caget/caput —— 用 auto_monitor 让 CA 线程收更新，
    get() 只读缓存；写用 wait=False。
    """

    def __init__(self):
        from epics import PV
        self._PV = PV
        self._pvs = {}

    def _pv(self, name):
        pv = self._pvs.get(name)
        if pv is None:
            pv = self._PV(name, auto_monitor=True, connection_timeout=0.1)
            self._pvs[name] = pv
        return pv

    def get(self, name, default=None):
        try:
            v = self._pv(name).get(timeout=0.0)
        except Exception:
            return default
        return default if v is None else v

    def get_str(self, name, default=""):
        try:
            v = self._pv(name).get(as_string=True, timeout=0.0)
        except Exception:
            return default
        return default if v is None else str(v)

    def put(self, name, value):
        try:
            self._pv(name).put(value, wait=False)
            return True
        except Exception:
            return False


# ───────────────────────── config CLI ─────────────────────────
def run_cfg_cli(args):
    """调用 msm_cli_config，返回 (ok, dict)。"""
    if not os.path.exists(CFG_CLI):
        return False, {"status": "error:missing_cli",
                       "msg": "%s 不存在（先编译 msm_cli_config）" % CFG_CLI}
    try:
        p = subprocess.run([CFG_CLI] + args, capture_output=True, text=True,
                           timeout=15)
    except Exception as e:                                  # noqa: BLE001
        return False, {"status": "error:exec", "msg": str(e)}
    out = (p.stdout or "").strip()
    try:
        return (p.returncode == 0), json.loads(out) if out else {}
    except json.JSONDecodeError:
        return False, {"status": "error:bad_json", "msg": out[:400]}


# ───────────────────────── 配置对话框 ─────────────────────────
class ConfigDialog(QDialog):
    """对齐 webui 的 Config 面板：选卡 → 选通道 → 改参数 → 按 scope 保存。"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("MSM Config")
        self.resize(640, 620)
        self.meta = {}
        self.cfg = {}
        self._loading = False

        root = QVBoxLayout(self)

        top = QHBoxLayout()
        top.addWidget(QLabel("Card:"))
        self.cb_card = QComboBox()
        self.cb_card.currentIndexChanged.connect(self.on_card_changed)
        top.addWidget(self.cb_card, 2)
        top.addWidget(QLabel("Channel:"))
        self.cb_ch = QComboBox()
        self.cb_ch.currentIndexChanged.connect(self.load_channel)
        top.addWidget(self.cb_ch, 1)
        root.addLayout(top)

        self.table = QTableWidget(0, 2)
        self.table.setHorizontalHeaderLabels(["parameter", "value"])
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeToContents)
        self.table.horizontalHeader().setSectionResizeMode(1, QHeaderView.Stretch)
        self.table.verticalHeader().setVisible(False)
        root.addWidget(self.table, 1)

        btns = QHBoxLayout()
        self.btn_ch = QPushButton("Save Channel")
        self.btn_card = QPushButton("Save Card")
        self.btn_chassis = QPushButton("Save Chassis")
        self.btn_reload = QPushButton("Reload")
        for b in (self.btn_ch, self.btn_card, self.btn_chassis, self.btn_reload):
            btns.addWidget(b)
        btns.addStretch(1)
        self.btn_close = QPushButton("Close")
        self.btn_close.clicked.connect(self.accept)
        btns.addWidget(self.btn_close)
        root.addLayout(btns)

        self.btn_ch.clicked.connect(lambda: self.save("channel"))
        self.btn_card.clicked.connect(lambda: self.save("card"))
        self.btn_chassis.clicked.connect(lambda: self.save("chassis"))
        self.btn_reload.clicked.connect(self.reload)

        self.status = QLabel("")
        self.status.setWordWrap(True)
        root.addWidget(self.status)

        self.reload()

    # ── 状态行（绿=成功 / 红=失败 / 灰=中性）──
    def _set_status(self, text, kind="info"):
        color = {"ok": "#1e8e3e", "err": "#d93025", "info": "#606070"}[kind]
        icon = {"ok": "✓ ", "err": "✗ ", "info": ""}[kind]
        weight = "bold" if kind != "info" else "normal"
        self.status.setStyleSheet("color:%s; font-weight:%s" % (color, weight))
        self.status.setText(icon + text)

    # ── 数据加载 ──
    def reload(self):
        ok, meta = run_cfg_cli(["meta", "--config", CONFIG_FILE])
        if not ok:
            self._set_status("读取 meta 失败：%s" % meta.get("msg", meta), "err")
            return
        self.meta = meta
        ok, cfg = run_cfg_cli(["get", "--config", CONFIG_FILE])
        if not ok:
            self._set_status("读取配置失败：%s" % cfg.get("msg", cfg), "err")
            return
        self.cfg = cfg

        self._loading = True
        self.cb_card.clear()
        cards = cfg.get("DaqParameter", {}).get("Cards", {})
        labels = self.meta.get("cards", {})
        for key, card in cards.items():
            idx = card.get("CardIndex")
            ctype = str(card.get("CardType", ""))
            label = labels.get(ctype, ctype)
            self.cb_card.addItem("%s  [%d] %s" % (key, int(idx), label),
                                 int(idx))
        self._loading = False
        self.on_card_changed()
        self._set_status("Reload 成功：重新读取 %d 张卡 / %s"
                         % (len(cards), CONFIG_FILE), "ok")

    def _card(self):
        idx = self.cb_card.currentData()
        for _, card in self.cfg.get("DaqParameter", {}).get("Cards", {}).items():
            if int(card.get("CardIndex", -1)) == int(idx):
                return card
        return None

    def on_card_changed(self):
        if self._loading:
            return
        card = self._card()
        self._loading = True
        self.cb_ch.clear()
        if card:
            for key, ch in card.get("Channels", {}).items():
                self.cb_ch.addItem(key, int(ch.get("channel", 0)))
        self._loading = False
        self.load_channel()

    def _channel(self):
        card = self._card()
        if not card:
            return None
        want = self.cb_ch.currentData()
        if want is None:
            return None
        for _, ch in card.get("Channels", {}).items():
            if int(ch.get("channel", -1)) == int(want):
                return ch
        return None

    def load_channel(self):
        """把通道参数填进表格；枚举键用下拉框。"""
        if self._loading:
            return
        ch = self._channel()
        if ch is None:
            self.table.setRowCount(0)
            return
        enums = self.meta.get("enums", {})
        keys = [k for k in self.meta.get("keys", []) if k in ch] or list(ch.keys())
        self.table.setRowCount(len(keys))
        for r, key in enumerate(keys):
            name = QTableWidgetItem(key)
            name.setFlags(name.flags() & ~Qt.ItemIsEditable)
            self.table.setItem(r, 0, name)
            val = str(ch.get(key, ""))
            if key in enums:
                cb = QComboBox()
                cb.addItems([str(x) for x in enums[key]])
                if val in [str(x) for x in enums[key]]:
                    cb.setCurrentText(val)
                self.table.setCellWidget(r, 1, cb)
            else:
                self.table.setItem(r, 1, QTableWidgetItem(val))

    def collect(self):
        params = {}
        for r in range(self.table.rowCount()):
            key = self.table.item(r, 0).text()
            w = self.table.cellWidget(r, 1)
            if isinstance(w, QComboBox):
                params[key] = w.currentText()
            else:
                item = self.table.item(r, 1)
                params[key] = item.text() if item else ""
        return params

    def save(self, scope):
        card = self._card()
        ch = self._channel()
        if card is None or ch is None:
            self._set_status("没有可保存的卡/通道", "err")
            return
        args = ["save", "--config", CONFIG_FILE, "--scope", scope,
                "--card", str(int(card.get("CardIndex", 0))),
                "--channel", str(int(ch.get("channel", 0)))]
        for k, v in self.collect().items():
            args += ["--set", "%s=%s" % (k, v)]
        ok, res = run_cfg_cli(args)
        status = res.get("status", res)
        if ok and status == "ok":
            # 保存成功后重新读取，让表格回显落盘后的真实值
            self.reload()
            # reload 内部会写绿色状态，这里再覆盖为"保存成功"，
            # 让用户明确知道是保存动作的结果（而非单纯 reload）
            self._set_status(
                "保存成功（scope=%s, card=%s, ch=%s）→ %s，已回读确认"
                % (scope, card.get("CardIndex"), ch.get("channel"),
                   os.path.basename(CONFIG_FILE)), "ok")
        else:
            self._set_status(
                "保存失败：%s%s" % (
                    status, (" / " + res.get("msg", "")) if res.get("msg") else ""),
                "err")
            QMessageBox.warning(self, "保存失败",
                                "写入 %s 未成功：\n%s%s" % (
                                    CONFIG_FILE, status,
                                    ("\n" + res.get("msg", ""))
                                    if res.get("msg") else ""))


# ───────────────────────── 控制条 ─────────────────────────
class ControlBar(QWidget):
    def __init__(self, pvs, ctl, parent=None):
        super().__init__(parent)
        self.pvs = pvs
        self.ctl = ctl          # 已展开的 control PV 名
        self.child = None
        self.launched_at = 0.0
        self.crashed = False
        self._last_frames = None
        self._last_fps_t = 0.0
        self._save_touched = 0.0
        self.display = None     # build() 里注入

        self.setObjectName("controlBar")
        # 纯 QWidget 子类默认不绘制样式表背景，必须显式打开
        self.setAttribute(Qt.WA_StyledBackground, True)
        lay = QHBoxLayout(self)
        lay.setContentsMargins(8, 6, 8, 6)
        lay.setSpacing(8)

        title = QLabel("MSM DAQ")
        title.setObjectName("title")
        lay.addWidget(title)

        self.rb_hw = QRadioButton("HW")
        self.rb_sim = QRadioButton("SIM")
        self.rb_sim.setChecked(True)
        lay.addWidget(self.rb_hw)
        lay.addWidget(self.rb_sim)

        self.btn_run = QPushButton("▶ Run")
        self.btn_run.setObjectName("btnRun")
        self.btn_quit = QPushButton("■ Quit")
        self.btn_quit.setObjectName("btnQuit")
        lay.addWidget(self.btn_run)
        lay.addWidget(self.btn_quit)

        self.dot_backend = QLabel("●")
        self.dot_backend.setObjectName("dot")
        self.lbl_backend = QLabel("stopped")
        lay.addWidget(self.dot_backend)
        lay.addWidget(self.lbl_backend)

        lay.addWidget(self._sep())

        self.chk_save = QCheckBox("Save")
        self.chk_wave = QCheckBox("Waveform")
        self.chk_wave.setChecked(True)
        self.btn_start = QPushButton("▶ Start")
        self.btn_stop = QPushButton("■ Stop")
        self.btn_cfg = QPushButton("Config")
        lay.addWidget(self.chk_save)
        lay.addWidget(self.chk_wave)
        lay.addWidget(self.btn_start)
        lay.addWidget(self.btn_stop)
        lay.addWidget(self.btn_cfg)

        lay.addWidget(self._sep())
        self.dot_daq = QLabel("●")
        self.dot_daq.setObjectName("dot")
        self.lbl_daq = QLabel("DAQ")
        self.lbl_fps = QLabel("0 fps")
        lay.addWidget(self.dot_daq)
        lay.addWidget(self.lbl_daq)
        lay.addWidget(self.lbl_fps)
        lay.addStretch(1)

        self.btn_run.clicked.connect(self.on_run)
        self.btn_quit.clicked.connect(self.on_quit)
        self.btn_start.clicked.connect(lambda: self.pvs.put(self.ctl["start"], 1))
        self.btn_stop.clicked.connect(lambda: self.pvs.put(self.ctl["stop"], 1))
        self.chk_save.toggled.connect(self.on_save_toggled)
        self.chk_wave.toggled.connect(self.on_wave_toggled)
        self.btn_cfg.clicked.connect(self.on_config)

        self.timer = QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(500)
        self.refresh()

    def _sep(self):
        f = QFrame()
        f.setFrameShape(QFrame.VLine)
        f.setFrameShadow(QFrame.Sunken)
        return f

    # ── 状态 ──
    def backend_ready(self):
        return int(self.pvs.get(self.ctl["backend_alive"], 0) or 0) == 1

    def daq_running(self):
        return int(self.pvs.get(self.ctl["daq_running"], 0) or 0) == 1

    def refresh(self):
        ready = self.backend_ready()
        running = self.daq_running()
        launching = False
        if self.child is not None:
            rc = self.child.poll()
            if rc is None:
                launching = not ready and (time.time() - self.launched_at) < 40
            else:
                self.crashed = rc != 0
                self.child = None
        if launching:
            state, color = "launching", YELLOW
        elif self.crashed and not ready:
            state, color = "crashed", RED
        elif running:
            state, color = "running", GREEN
        elif ready:
            state, color = "idle", GREEN
        else:
            state, color = "stopped", GREY
        if ready:
            self.crashed = False

        self.lbl_backend.setText(state)
        self.dot_backend.setStyleSheet("color:%s" % color)
        self.dot_daq.setStyleSheet("color:%s" % (GREEN if running else GREY))

        # Save 勾选框回读同步（用户刚点过 1.5s 内不覆盖，避免写-读竞态抖动）
        if ready and time.time() - self._save_touched > 1.5:
            v = int(self.pvs.get(self.ctl["save_enable"], 0) or 0)
            if self.chk_save.isChecked() != bool(v):
                self.chk_save.blockSignals(True)
                self.chk_save.setChecked(bool(v))
                self.chk_save.blockSignals(False)

        self.btn_run.setEnabled(not ready and not launching)
        self.btn_quit.setEnabled(ready or self.child is not None)
        self.btn_start.setEnabled(ready and not running)
        self.btn_stop.setEnabled(ready and running)
        self.chk_save.setEnabled(ready)
        self.btn_cfg.setEnabled(os.path.exists(CFG_CLI))
        self.lbl_fps.setText("%d fps" % self._fps())

    def _fps(self):
        v = self.pvs.get(self.ctl["fps_source"], None)
        if v is None:
            return 0
        now = time.time()
        fps = 0
        if self._last_frames is not None and now > self._last_fps_t:
            fps = int((float(v) - self._last_frames) / (now - self._last_fps_t))
        self._last_frames = float(v)
        self._last_fps_t = now
        return max(fps, 0)

    # ── 动作 ──
    def on_run(self):
        mode = "sim" if self.rb_sim.isChecked() else "hw"
        binary = "msm_cli_simsrc" if mode == "sim" else "msm_cli_hwdaq"
        path = os.path.join(CLI_DIR, binary)
        if not os.path.exists(path):
            QMessageBox.critical(self, "Run", "找不到 %s" % path)
            return
        try:
            self.child = subprocess.Popen([path], cwd=ROOT,
                                          start_new_session=True)
        except Exception as e:                              # noqa: BLE001
            QMessageBox.critical(self, "Run", "启动失败：%s" % e)
            return
        self.launched_at = time.time()
        self.crashed = False
        self.refresh()

    def on_quit(self):
        self.pvs.put(self.ctl["quit"], 1)
        child = self.child
        if child is None:
            return
        for _ in range(100):
            if child.poll() is not None:
                break
            time.sleep(0.1)
        else:
            child.terminate()
            try:
                child.wait(timeout=5)
            except Exception:                               # noqa: BLE001
                child.kill()
        self.child = None
        self.refresh()

    def on_save_toggled(self, on):
        self._save_touched = time.time()
        self.pvs.put(self.ctl["save_enable"], 1 if on else 0)

    def on_wave_toggled(self, on):
        if not self.display:
            return
        for w in self.display.findChildren(QWidget):
            if w.objectName().startswith("wavePanel"):
                w.setVisible(on)

    def on_config(self):
        dlg = ConfigDialog(self)
        dlg.exec_()


# ───────────────────────── main ─────────────────────────
def build():
    """构造 (app, window)，不进入事件循环（离屏截图也复用这条路径）。"""
    env = load_prefix()
    cfg = load_pv_map()

    # CA 环境（run_pydm.sh 也会设，这里兜底）
    ioc = cfg.get("ioc", {}).get("addr_list", "127.0.0.1:5064")
    os.environ.setdefault("EPICS_CA_ADDR_LIST",
                          os.environ.get("MSM_IOC_ADDR", ioc.split(":")[0]))
    os.environ.setdefault("EPICS_CA_AUTO_ADDR_LIST", "NO")

    from pydm import PyDMApplication, Display

    app = PyDMApplication(use_main_window=False)

    ctl_raw = cfg.get("control", {})
    ctl = {}
    for item in ctl_raw.get("commands", []):
        name = item.get("cmd", "").lower()
        ctl[name] = expand(item["pv"], env)
    ctl["save_enable"] = expand(ctl_raw.get("save_pv", {}).get("pv", ""), env)
    ctl["frame_index"] = expand(ctl_raw.get("frame_index_pv", {}).get("pv", ""), env)
    st = ctl_raw.get("status", {})
    ctl["backend_alive"] = expand(st.get("backend_alive", ""), env)
    ctl["daq_running"] = expand(st.get("daq_running", ""), env)
    ctl["resp"] = expand(st.get("resp", ""), env)
    ctl["fps_source"] = expand("${P}${I}DAQ:ch1_frames", env)

    pvs = Pvs()
    bar = ControlBar(pvs, ctl)

    if not os.path.exists(UI_FILE):
        raise SystemExit("缺 %s（先 ./run_pydm.sh --regen）" % UI_FILE)

    display = Display(ui_filename=UI_FILE,
                      macros={"P": env["P"], "I": env["I"]})
    bar.display = display

    # Frame Index + Set（.ui 里是普通 Qt 控件，逻辑在这里接）
    edit = display.findChild(QLineEdit, "editFrameIndex")
    btn = display.findChild(QPushButton, "btnSetFrameIndex")
    if edit is not None and btn is not None:
        def set_index():
            txt = edit.text().strip()
            if not txt:
                return
            try:
                idx = int(txt)
            except ValueError:
                QMessageBox.warning(None, "Frame Index", "请输入整数")
                return
            pvs.put(ctl["frame_index"], idx)
        btn.clicked.connect(set_index)
        edit.returnPressed.connect(set_index)

    win = QMainWindow()
    win.setWindowTitle("MSM DAQ")
    central = QWidget()
    lay = QVBoxLayout(central)
    lay.setContentsMargins(0, 0, 0, 0)
    lay.setSpacing(0)
    lay.addWidget(bar)
    lay.addWidget(display, 1)
    win.setCentralWidget(central)
    win.resize(1180, 980)

    win.setStyleSheet("""
        QWidget#controlBar { background:#1d1d24; border-bottom:1px solid #3a3a48; }
        QWidget#controlBar QLabel { color:#d0d0da; }
        QWidget#controlBar QLabel#title { font-weight:bold; font-size:14px;
                                          color:#e8e8f0; padding-right:6px; }
        QWidget#controlBar QLabel#dot { font-size:15px; }
        QWidget#controlBar QPushButton { background:#2e2e3a; color:#e0e0e8;
                                         border:1px solid #45455a; border-radius:4px;
                                         padding:4px 10px; }
        QWidget#controlBar QPushButton:hover { background:#3a3a4a; }
        QWidget#controlBar QPushButton:disabled { color:#66666f;
                                                  border-color:#33333d; }
        QWidget#controlBar QPushButton#btnRun { background:#2f6b45; }
        QWidget#controlBar QPushButton#btnQuit { background:#6b2f2f; }
        QWidget#controlBar QCheckBox, QWidget#controlBar QRadioButton {
                                          color:#dcdce6; spacing:5px; }
        QWidget#controlBar QCheckBox::indicator, QWidget#controlBar QRadioButton::indicator {
                                          width:13px; height:13px;
                                          border:1px solid #6a6a7a; border-radius:2px;
                                          background:#15151b; }
        QWidget#controlBar QCheckBox::indicator:checked {
                                          background:#3ec46d; border-color:#3ec46d; }
        QWidget#controlBar QRadioButton::indicator:checked {
                                          background:#4a8fe0; border-color:#4a8fe0; }
    """)

    win.show()
    return app, win


def teardown(app, win):
    """事件循环退出后显式断开全部 PyDM 通道（必须在进程退出前做）。

    不这么做的话：解释器收尾阶段 PyQt5/sip 按模块序销毁 C++ 对象，
    Connection（pydm 插件里持有的 QObject）的 C++ 先被删，而各 PyDM 控件
    的 destroyed 信号回调稍后才跑 channel.disconnect() →
    plugin.remove_connection() 去 deleteLater() 已死的 C++ 对象 →
    每个 PV 一条 "RuntimeError: wrapped C/C++ object of type Connection
    has been deleted"（实测 50+ 条刷屏）。
    此刻对象都还活着：正常断开 + 清空插件连接表，退出时回调全部空操作。
    """
    try:
        from pydm.utilities import close_widget_connections
        close_widget_connections(win)
    except Exception:                              # noqa: BLE001
        pass
    try:
        import pydm.data_plugins as dp
        pl = dp.plugin_modules.get("ca")
        if pl is not None:
            for conn in list(pl.connections.values()):
                try:
                    conn.close()
                except Exception:                  # noqa: BLE001
                    pass
            pl.connections.clear()
            pl.channels.clear()
    except Exception:                              # noqa: BLE001
        pass


def main():
    app, win = build()
    rc = app.exec_()
    teardown(app, win)
    return rc


if __name__ == "__main__":
    sys.exit(main())
