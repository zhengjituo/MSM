# Task 021 — PyDM 控制界面与 webui 操作逻辑对齐

## 目标

PyDM 侧从"只能看"变成"能操作"，与 `main_webui` 对齐：选 HW/SIM 起 backend、
Run/Quit、Start/Stop、Save 开关、波形显隐、Frame Index + Set、Config 编辑。

IOC 仍由外部 `run_epics.sh` 启动；PyDM 与 webui **地位对等、互斥使用**。

## 架构（关键约束）

```
backend ──IPC── msm_epics(桥) ──CA── PyDM(msm_app.py)
                     │
                     └── 只读 IPC 状态 / 转发控制命令
```

- `IPCServer::handleClient` 是**单客户端阻塞 accept**，桥常驻占用。
  所以 PyDM **绝不**直连 `/tmp/msm_daq.sock`，否则桥会被顶掉（反之亦然）。
- 于是所有控制都走 PV：PyDM 写 PV → 桥订阅 → 转成 IPC 命令 → 回写 `resp`。
- config 编辑是纯文件操作，与 backend 无关：`msm_cli_config` 复用
  `ConfigEditorModule` 的 `importConfig/applyEdit/getConfigJson`。
- 保存写的是 `config/app_pending_config.json`；backend 启动时才把它复制成
  `app_runtime_config.json`，所以**改动下次 Run 生效**，不热推（与 webui 一致）。

## pv_map.json 的 control 段（唯一真源）

| PV | 类型 | 方向 | 语义 |
|---|---|---|---|
| `CTRL:start` / `stop` / `quit` | bo | PyDM→桥 | 写 1 触发，桥转发后**复位为 0** |
| `CTRL:save_enable` | bo | PyDM→桥 | 1=ENABLESAVE，0=DISABLESAVE（状态位，不复位） |
| `CTRL:msg` | stringout | PyDM→桥 | 原样转发（保留通道，当前未用） |
| `CTRL:set_frame_index` | longout | PyDM→桥 | 桥拼 `MSG SetVar >> var=frame_index,value=N -@ Process`，处理后复位 -1 |
| `CTRL:backend_alive` / `CTRL:daq_running` | bi | 桥→PyDM | 状态灯（backend 连上 / 采集中） |
| `CTRL:resp` | stringin | 桥→PyDM | 最后一条 IPC 响应（调试用） |

`.db` 与 `.ui` 都由 `gen_db.py` / `gen_pydm.py` 从 pv_map 派生，不要手改产物。

## 踩过的坑（都实测过，别重复踩）

1. **`bo` 记录不能显式写 `field(HIGH, "1")`**。EPICS 7.0.8.1 下写 1 后
   **约 1 秒 VAL 自己弹回 0**；去掉该字段就正常（HIGH 默认值本来就是 1）。
   最小复现：同一 IOC 里四个记录，只有带 `field(HIGH,"1")` 的那个会回弹。
2. **`stringout` 的 VAL 只有 40 字符**（`MAX_STRING_SIZE`）。`MSG SetFrameIndex
   >> index=42 -@ Process` 是 41 字符，put 直接报 `Invalid string`。
   所以 frame index 用 `longout` 传数值、由桥拼命令；`resp` 回写也截到 39 字符。
3. **CA 的值去重**：同一个值连续写两次，第二次不产生 monitor 事件。所以
   "写 1 触发" 的 PV 必须由桥复位（start/stop/quit→0，frame_index→-1）。
4. **订阅会立刻投递一次初值**，桥对首批事件全部跳过，否则启动瞬间会发出一条
   假命令（例如 DISABLESAVE / SetFrameIndex 0）。
5. **backend 没有实现 `SetFrameIndex` 命令**（webui 的 Set 按钮一直是空操作）。
   这里改走 Module 内建的 `SetVar` 写 `frame_index`，语义等价且真的生效。
   NIStringParser 的键值对是**逗号分隔**：`var=frame_index,value=42`（空格不行）。
6. **PyQt 纯 QWidget 子类不画样式表背景**，控制条必须
   `setAttribute(Qt.WA_StyledBackground, True)` 才有深色底。
7. **GUI 线程不做阻塞 CA**：用 `epics.PV(auto_monitor=True)` 收 monitor，
   `get(timeout=0)` 只读缓存，写用 `wait=False`。
8. 同一前缀只允许一个 `softIoc`；多实例会让 CA 客户端随机连到不同实例
   （表现为读数在两个值之间跳）。`run_epics.sh` 已有单实例清理。

## 新增/修改文件

- `config/pv_map.json`：新增 `control` 段。
- `main_epics/gen_db.py`：`gen_bo`（去掉 HIGH）、`gen_longout`、`gen_stringout`、
  `gen_stringin`，并在 `main()` 里生成控制记录。
- `main_epics/main_epics.cpp`：控制通道订阅（`ctrlEventCb`）、主循环分发
  （`ctrlDrain`，不在 CA 回调线程里做阻塞 IPC）、状态回写
  （`ctrlPublishStatus`）、`caPut` 支持 string/longout。
- `main_cli/msm_cli_config.cpp` + `CMakeLists.txt`：新增 CLI
  `meta | get | save --scope channel|card|chassis --set k=v | save-app`，
  stdout 只输出 JSON，退出码 ok=0 / locked=2 / 其它=1。
- `main_epics/gen_pydm.py`：帧数表加 `Batches` 列、特征表上方加 `Index + Set` 行。
- `main_epics/msm_app.py`（新）：`PyDMApplication` + 自建 QMainWindow =
  顶部控制条 + 内嵌 `pydm.Display(msm_daq.ui)`；`build()` 拆出来供离屏截图复用。
- `main_epics/run_pydm.sh`：入口改为 `python msm_app.py`（找不到则退回原 `pydm <ui>`）。

## 验证记录（2026-11-13，sim）

| 项 | 结果 |
|---|---|
| Run(sim) → backend 就绪 | 状态 `launching → idle`，child pid 回收正常 |
| Start / Stop | `daq_running` On/Off，Stop 后帧数 3s 内归零；重复 Start 仍生效 |
| Quit | backend 退出，无残留 `msm_cli_simsrc` |
| Save 开关 | 写 1 保持 `Enable`（+4s 仍为 1），resp `enablesave sent`；写 0 → `disablesave sent` |
| Frame Index | 输入 42/7/321 → `Process:frame_index` = 42/7/321，resp `ok:1` |
| Config | 选卡/选通道/改参 → `Save Channel` 只改该通道；`Save Card` 4 通道全改 |
| 波形显隐 | 勾掉后 wavePanel 全部隐藏，勾回恢复 |
| 波形数据 | ch1–ch4 严格递增 0..65408，离屏截图为直线（无毛刺） |
| 离屏渲染 | `1180×1005`，布局与 webui 一致（帧数表 / 特征表 / 2×2 波形） |

## 使用

```bash
./main_epics/run_epics.sh            # 外部起 IOC + 桥（单实例、写 pid）
./main_epics/run_pydm.sh             # 起界面（首次或改过 pv_map 加 --regen）
# 界面里选 HW/SIM → ▶ Run → ▶ Start；Config 改完参数，下次 Run 生效
```

验证编译：

```bash
cmake /mnt/hgfs/share/opencode/MSM -B /tmp/opencode/msm_build_proc
make -C /tmp/opencode/msm_build_proc msm_epics msm_cli_config -j4
```

## 未做 / 后续

- `CTRL:msg`（通用字符串命令）保留了记录与桥分发逻辑，但 40 字符限制让它
  不适合长命令；目前没有使用者。
- `msm_cli_config save-app` 对数字字段会走 `strtod`，`runId:147 → 99.0`（模块既有语义）。
- **尚未 git commit / push**，等用户亲自验收后再整理提交。
