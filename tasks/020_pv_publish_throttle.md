# Task 020 — PV 推送限频（2–10Hz）+ 投影单向化

## 备份 git 号（恢复点）
```
4fee053  docs(plan): config pending-chain implementation plan (task 077)
```
> 开发中如需回退：`git reset --hard 4fee053`。
> 本地旧历史另存为分支 `backup/local-89d7e40`（同步前的 SHINE_CSM 时代工作）。

## 背景

用户需求：`module_process` 需要向外推 PV，但必须**限制在 2–10Hz**。

现状调研（2026-11-13）：

1. `module_process` 已经迁移到 `declareVar` 模式（`registerVars()` 里声明
   `chN_frames / chN_batches / chN_trig_id / chN_bunch_id / chN_sum_h` 以及
   `x_pos / y_pos / intensity`），**但已经不发 `PV_UPDATE` 了**。
2. `main_cli` / `main_webui` **根本没有实例化 PVServer**，PV 通道现在是空的。
   只有 `module_core/tests/calculator` 还在用 PVServer。
3. `PVServer::pushToIOC()` 的实现是**推送一次 = 新建 CA channel + 等连接 +
   `ca_pend_io(1.0)` + `ca_clear_channel`**，`pushAllToIOC()` 还在每个 PV 之间
   `sleep(10ms)`。按 16 通道 × 8 变量 ≈ 128 个 PV 计，一轮全量推送要十几秒。
   **这就是为什么当初必须"限频"——不是需求，是性能缺陷逼出来的。**
4. `PVServer::onMonitorUpdate()` 把外部 `pvput` 直接 `writeMemory()` 写进模块
   内存，**绕过 MessageBus**，与模块自身的 `handleState` 逻辑并行，形成第二真值源。

## 方案（2026-11-13 用户确认）

三件事一起做：

- **A. 独立 PV 推送节拍器**（用户选"模块侧节流/统一限频"）
  - 业务模块只管更新内存，不再发 PV 消息。
  - 由 `PVServer` 内部一个固定周期线程，按可配置间隔（默认 285ms ≈ 3.508Hz，
    可配 100–500ms 即 2–10Hz）批量扫所有 PV，**只推送值发生变化的**。
  - 值未变则跳过（配合修好的 `lastPushed*` 指纹）。
- **B. 修复 `pushToIOC` 性能**：复用 `caChannels_` 里的常驻 channel 做 put，
  不再每次 create/clear；去掉 `sleep(10ms)` 与 `ca_pend_io(1.0)` 的阻塞等待。
- **C. 投影单向化**：外部 `pvput` → 不再直接 `writeMemory`，改为投
  `Message("SetVar")` 给目标模块，由模块 `handleState` 处理；模块改了值之后
  由节拍器自然推送回去。写回闭环用"推送指纹 vs 接收指纹"消环。

## 配置

`config/app_initial_config.json` 增加（可选，缺省也能跑）：

```json
"PV": {
    "enabled": true,
    "prefix": "MSM",
    "rate_hz": 3.5,
    "epics_base": "",
    "db_path": "msm_pvs.db"
}
```

`rate_hz` 会被钳制到 `[2, 10]`。

## 最终实现（2026-11-13）

需求经多轮澄清后定型为：**独立进程 `main_epics` 做外部 IOC 的 CA client**，
PyDM 独立连 IOC，`main_epics` 不启动 IOC、不负责 PyDM。

### 交付物

| 文件 | 作用 |
|---|---|
| `config/pre.cfg` | **前缀唯一真源**（`P=`/`I=`），三处消费者共享 |
| `config/pv_map.json` | **PV 白名单唯一人写处**：暴露哪些变量、PV 名、频率分组、EGU/PREC |
| `main_epics/gen_db.py` | pv_map → `msm_pvs.db`（保留 `$(P)$(I)` 宏，支持多实例） |
| `main_epics/gen_pydm.py` | pv_map → `msm_daq.ui`（PyDM 界面，对标 webui） |
| `main_epics/main_epics.cpp` | IPC → CA put 桥；分层节拍 + 变化检测 + 常驻 channel |
| `main_epics/run_epics.sh` | 启动脚本：唯一前缀注入点，串起 IOC + msm_epics（+ 可选 pydm） |
| `module_core` 改动 | `VarTable` 增加 `publish` 元信息；`MessageBus::modules()`；`Module::setVarByName` + 内建 `SetVar` 分发 |

### 分层限频（用户要求 2–10Hz）

`pv_map.json` 的 `groups` 每项带 `rate_hz`（钳制到 [2,10]）：

| 组 | rate | 内容 |
|---|---|---|
| fast | 10 Hz | `x_pos` / `y_pos` / `intensity` |
| slow | 2 Hz | 计数器 + 4 通道特征表（min/max/sum_l/sum_h/vpp/trig_id/bunch_id） |
| wave | 5 Hz | 4 通道波形（waveform record, NELM=4096） |

### 关键设计决策

1. **不做内部 IOC**：`main_epics` 纯 CA client，IOC 由 `run_epics.sh` 独立启动。
   好处：MSM 重启不影响 PyDM 连接；`.db` 是部署物可人工审查。
2. **`SCAN=Passive`**：record 不自行扫描，值只由 MSM push。
3. **`ADEL=0`**：每次 put 都产生 monitor 事件，PyDM 不漏更新。
4. **变化检测**：值未变不推，节省 CA 带宽。
5. **常驻 channel**：`ca_array_put` + `ca_flush_io`，不每次 create/clear、不阻塞等待。
6. **前缀走 `pre.cfg`**：同一份 `.db` / `.ui` 部署多装置，启动时传 `-m "P=..,I=.."`。

### 验证结果（2026-11-13，sim 数据源）

```
群组解析:  fast @ 10Hz (3 vars) / slow @ 2Hz (38 vars) / wave @ 5Hz (4 vars)
CA 连接:   45/45 channels connected
标量读数:  ch1_frames=840000  batch_no=84  ch1_min=114.261  ch1_max=115.303
波形读数:  wave_ch1 = 0 512 9385 1539 ...（4096 元素）
频率实测:  slow  10s → 20 次（2.0 Hz）
           wave  10s → 47 次（4.7385 Hz）
```

### 已知限制

- PyDM 的 `.ui` 已生成并通过 XML 校验，但**当前环境无 pydm/PyQt5，未做渲染验证**；
  需在现场安装 pydm 后确认控件显示。
- 波形 record 用 `FTVL=ULONG`，避免 `uint16` 放 SHORT 时符号位翻转。
- `x_pos`/`y_pos`/`intensity` 在 sim 数据里恒为 0，故 fast 组实测无更新（变化检测生效，
  非缺陷）；接真实硬件或更有变化的 sim 数据后可见 10Hz 更新。

## TODO（已完成）

- [x] T1 分层节拍（fast/slow/wave 各自 deadline）
- [x] T2 常驻 channel 推送（无 create/clear、无阻塞）
- [x] T3 变化检测（值未变不推）
- [x] T4 `Module::setVarByName` + 内建 `SetVar` 分发（投影单向化的基础）
- [x] T5 `VarTable` 增加 `publish` 元信息（默认 true，可关）
- [x] T6 `main_epics` 独立进程（不依赖 backend 编译期链接）
- [x] T7 编译 + sim 端到端验证（标量 + 波形 + 频率）

## 未完成 / 后续

- [ ] `PVServer` 与 `main_epics` 功能重叠，`PVServer` 现仅 calculator 测试使用，
      建议后续标记为 legacy 或删除
- [ ] PyDM `.ui` 现场渲染验证
- [ ] 命令 PV（`Cmd:start/stop/save`）——本轮未做，PyDM 仅只读
- [ ] `onMonitorUpdate` 回写仍走 `writeMemory`（投影单向化只做到 `SetVar` 入口，
      未接 `main_epics` 的反向通道）

## 硬性约束（用户 2026-11-13 指令）

> **在用户亲自测试通过之前，禁止向 GitHub 推送任何更新。**

- 禁止：`git push`、`git push --force`、创建 PR、任何写入 `origin` 的操作。
- 允许：本地 commit / branch / 编译 / 运行 / 测试。
- 解除条件：仅当用户明确说"测试通过，可以推"之后才可推送。

## 补充验证（2026-11-13 深夜，用户要求）

### 1. PyDM 环境安装（本机原无）

```bash
uv venv /tmp/opencode/pydm-venv --python 3.10
uv pip install --python /tmp/opencode/pydm-venv/bin/python pydm PyQt5
# 结果: pydm 1.29.0 + PyQt5 5.15.11
```

### 2. 新增 DAQ / Storage 帧数 PV（4 通道）

用户指出必须验证"4 通道的 process 和送往 storage 的帧数"。

`pv_map.json` slow 组补充：

| 前缀 | 变量 | 来源 |
|---|---|---|
| `MSM:DAQ:chN_frames` | DAQ 采集帧 | `module_daq::onStatus` |
| `MSM:Process:chN_frames` | 处理帧 | `module_process::onStatus` |
| `MSM:Process:chN_batches` | 批数 | 同上 |
| `MSM:Storage:chN_saved_frames` | 存储消费帧 | `module_storage::onStatus` |

`main_epics` 改为轮询三个模块并合并为命名空间表（`DAQ:` / `Process:` / `Storage:`）。

### 3. 验证结果

```
=== 4 通道三组对比（同一时刻）===
DAQ      : 2.9385e+06 ×4 通道一致
Process  : 2.9385e+06 ×4 通道一致
Storage  : 2.9385e+06 ×4 通道一致
Process batches: 235 ×4 通道一致
```

`cainfo` 确认 record 为 `DBF_DOUBLE`，帧计数（百万级）无精度损失。

### 4. PyDM 端到端验证

```
PyDMLabel count: 30
live labels with values: 30/30
waveform len: 4096, nonzero: 511 (TrigLength=512，符合预期)
```

实时读数示例：

```
MSM:Process:batch_no      = 3695
MSM:Process:ch1_trig_id   = 36950000
MSM:Process:ch1_bunch_id  = 369499991
MSM:Process:ch1_min       = 114.2611
MSM:Process:ch1_max       = 115.4427
MSM:Process:ch1_sum_h     = 1.58792e+04
MSM:Process:ch1_vpp       = 9385.1816
```

### 5. 途中修掉的真实缺陷

| 缺陷 | 原因 | 修复 |
|---|---|---|
| `.ui` 无法加载 | `<rect>` 用了 XML 属性而非子元素 | 改成 `<x>/<y>/<width>/<height>` 子元素 |
| `QPyDMLabel` 找不到 | 误加 Qt 前缀 | 改为 `PyDMLabel` |
| 缺少控件声明 | 未写 `<customwidgets>` | 补 class/extends/header |
| 宏不展开 | 用了 `$(P)`（EPICS 语法） | PyDM 用 `${P}`（Python Template） |
| `ch1_min/max` 推送为 0 | 合并表带命名空间后键不匹配 | `lookupVar` 增加 `Process:` 前缀查找 |
| 波形 PV 全 0 | 波形键同样未带命名空间 | 同上，优先查 `Process:chN_feat_wave_raw` |
| `PyDMWaveformTable` 报错 | 该控件期望数组但绑定方式不对 | 波形改用 `PyDMWaveformPlot`（title 属性） |
| 通道表 `setChannel` 不存在 | 误用 PyDMTableView 属性 | 改为 `QTableWidget` + rowCount/columnCount |

### 6. 已知语义问题（待用户决策，未自行修改）

`module_storage` 的 `chN_saved_frames` 实际是 `framesConsumed`，
在 `save not enabled` 的 discard 路径下**也会累加**。
因此该 PV 名有误导性：它表示"已消费帧"，不是"已落盘帧"。
建议后续改名为 `chN_consumed_frames`，或让 discard 路径不计数。

### 7. 验证命令（复现）

```bash
# backend（sim）
mkfifo /tmp/sf; ( sleep 9365; echo start; sleep 2900 ) > /tmp/sf &
./main_cli/build/msm_cli_simsrc < /tmp/sf &

# IOC + 桥
./main_epics/run_epics.sh &

# 校验
$EPICS_BASE/bin/linux-x86_64/caget MSM:DAQ:ch1_frames MSM:Process:ch1_frames \
    MSM:Storage:ch1_saved_frames
$EPICS_BASE/bin/linux-x86_64/caget -#6 MSM:Process:wave_ch1
```

## 补充：改名 + Status 自动生成（2026-11-14）

### 1. `saved_frames` → `consumed_frames`（用户确认方案 b）

理由：`module_storage::onStatus` 输出的 `chN_saved_frames` 实为
`frames_[ch].framesConverted`……

### 2. `MSG Status` 基类自动生成
`Module::runLoop` 内置 `MsgStatus` 处理：先 `getVars()->list()` 全量导出 VarTable，
再 `onStatus()` 供子类补充特征行。子类不再手写 Status JSON。

### 验证
- `MSM:Storage:chN_consumed_frames` 有值，`saved_frames` 不存在（已配对）
- 53/53 channel connected（比之前 45 多 8 = DAQ 4 + Storage 4）
- `Status` 自动导出使 `MSM:DAQ:chN_frames` 等 PV 直接可读

## 补充：一键启动脚本 + 端到端实测（2026-11-13）

### 新增 `main_epics/run_sim_stack.sh`

三段一键（backend sim + IOC + IPC→CA 桥），PyDM 不在其中：

```bash
./main_epics/run_sim_stack.sh start     # 起三段，并自动给 backend 发 start
./main_epics/run_sim_stack.sh status    # 进程 + 抽样 PV
./main_epics/run_sim_stack.sh stop      # 全停
```

实现要点：
- backend 的 stdin 是控制通道（`getline` 读 `start`，EOF 即退出），故用
  `0<> /tmp/msm_sim.cmd` 以读写方式打开 FIFO，既拿到 stdin 又不会 EOF。
- 启动前先删残留 `/tmp/msm_daq.sock`，否则新 backend bind 失败。
- IOC / 桥 复用 `run_epics.sh`（前缀注入与 `.db` 生成仍在那一处），
  并补写 `/tmp/msm_ioc.pid`、`/tmp/msm_epics.pid` 供 stop 使用。

### 实测结果（sim 源）

```
backend   RUNNING        epics RUNNING        ioc RUNNING
[epics] channels connected 53/53
[epics] running; ipc=/tmp/msm_daq.sock

MSM:DAQ:ch1_frames                 500000
MSM:Process:ch1_frames             490000
MSM:Storage:ch1_consumed_frames    490000
MSM:Process:x_pos                  0
MSM:Process:wave_ch1               NORD=4096, [512, 1025, 1539, ...]
```

- `x_pos/y_pos/intensity/sum_l` 为 0 属 sim 无标定表的既有行为，
  与 `main_webui` 的 feature 表一致（同一份 `calibrate()` 输出），非桥接问题。
- `stop` → `start` 循环两次均正常，无残留进程/端口/socket。
- IPC 为单客户端：桥常驻占用 `/tmp/msm_daq.sock` 时，第二个客户端拿不到响应，
  因此不要同时开 `main_webui` 与 EPICS 桥（两类 UI 互斥）。

## 补充：PyDM 启动脚本 + Qt 插件缺库（2026-11-13）

### 症状
直接跑 `pydm main_epics/msm_daq.ui` 报：

```
qt.qpa.plugin: Could not load the Qt platform plugin "xcb" in "" even though it was found.
Aborted (core dumped)
```

### 根因
`libqxcb.so` 的依赖里缺 `libxcb-xinerama.so.0`（Ubuntu 22.04 未装 `libxcb-xinerama0`，
且本机无 root）。用 `ldd .../platforms/libqxcb.so | grep "not found"` 确认。

### 处理：新增 `main_epics/run_pydm.sh`
无需 sudo，按顺序自愈：
1. 系统已有 → 直接用
2. `~/.cache/msm/xcb` 有 → 注入 `LD_LIBRARY_PATH`
3. 有网 → `apt-get download libxcb-xinerama0` + `dpkg-deb -x` 解包到该缓存目录
4. 都不行 → `QT_QPA_PLATFORM=wayland`（本机实测可用）

同时从前缀唯一真源 `pre.cfg` 注入 `--macro "P=$P,I=$I"`。

### 验证
- 清空缓存后首次运行：自动下载解包成功，窗口正常，运行 30s 无崩溃（timeout 返回 124）
- 二次运行：命中缓存，输出「用缓存库」，不重复下载
- 无需改系统、无需 root，缓存落在 `~/.cache/msm/xcb`

### 用法
```bash
./main_epics/run_pydm.sh           # 起界面
./main_epics/run_pydm.sh --regen   # 先从 pv_map.json 重生成 msm_daq.ui
```

## 补充：PyDM 全灰的两个真因（2026-11-13）

### 现象
窗口能弹出来，但所有 PV 标签灰色无数据。

### 真因 1（决定性）：pydm 的宏解析器会丢掉**末尾空值宏**
`--macro "P=MSM:,I="` 经 `pydm.utilities.macro.parse_macro_string`
解析后只剩 `{'P': 'MSM:'}` —— `I` 被丢弃。追踪状态机：`I=` 是最后一个
键且值为空，`val_start` 从未被赋值，故整条被跳过。

后果：`.ui` 里 `${I}` 原样残留，通道地址变成
`ca://MSM:${I}Process:batch_no`，PyDM 连不上 → 全灰。

修复：改用 **JSON 形式**传宏（`parse_macro_string` 的优先分支，空串能保留）：

```bash
--macro '{"P": "MSM:", "I": ""}'
```

### 真因 2：IOC 只监听 127.0.0.1，而 CA 广播发现不覆盖回环
`run_epics.sh` 设了 `EPICS_CAS_INTF_ADDR_LIST=127.0.0.1`，客户端若不显式
指定地址就找不到 IOC。实测：

| 客户端环境 | caget 结果 |
|---|---|
| 默认（无 ADDR_LIST） | `cannot connect` / None |
| `EPICS_CA_ADDR_LIST=127.0.0.1` | 531290000 |

修复：`run_pydm.sh` 注入 `EPICS_CA_ADDR_LIST`（可用 `MSM_IOC_ADDR` 覆盖），
并把 `run_epics.sh` 的监听地址改成可配置（`MSM_CAS_INTF`，默认仍是回环）。

### 真因 3：`.ui` 自身两处结构错误（导致排版错乱 + 波形空白）
1. 网格布局写成了 `<item row><item><widget>`，多套一层 `<item>` 让行列号失效，
   所有控件退化成顺序堆叠，标签和数值错位。
2. 波形图只设了 `title`，没设通道。PyDM 的 `PyDMWaveformPlot` 在 `.ui` 里
   绑曲线用的是 **`curves`** 属性（`QStringList`，每项一条 JSON，
   键为 `y_channel` / `x_channel` / `plot_style` / `name`），
   不是 `channel` 也不是 `title`。

`gen_pydm.py` 已重写：帧数表 + 特征表改为正确的 `QGridLayout`，波形图写入
`curves`。同时把原来那个空的 `QTableWidget`（无法绑 PV）换成 `PyDMLabel` 网格。

### 波形横坐标：NORD 恒等于 NELM 的问题
桥原来固定写满 `nelm`（4096）个元素、不足补 0，CA 写数组的元素个数就是 NORD，
于是 NORD 恒为 4096，PyDM 横坐标被拉到 0–4095，而真实采样只有 TrigLength 点，
曲线挤在最左边一小条。

修复（`main_epics.cpp: caPutWave`）：只写实际长度 n，`NORD` 随真实采样数变化；
`NELM` 仍由 `.db` 固定为 4096，记录容量不变。

验证：
```
MSM:Process:wave_ch1.NORD = 512
MSM:Process:wave_ch1.NELM = 4096
pyepics 读到的长度 512，非零 511，最后非零下标 511
```

### 布局：每卡一个 tab + 卡内波形 2×2（对齐 webui）
webui 的波形区是 `grid-template-columns: 1fr 1fr`，即 2×2；卡片信息在
`config/app_runtime_config.json` 的 `DaqParameter.Cards`。

`gen_pydm.py` 改为：

- `QTabWidget`，每卡一个 tab，标题 `Card N (CHx–CHy)`
- 每个 tab 内：帧数表（仅本卡通道）+ 特征表 + 波形区
- 波形区用 `QGridLayout` 排成 2×2（`divmod(i, 2)`）
- 通道→卡片的划分由 `--ch-per-card` 控制（默认 4），
  通道总数由 `--nchannels` 控制（默认 4 → 单 tab）

多卡时 PV 命名沿用连续通道号（卡 2 = CH5–CH8），因此 `pv_map.json`
需要相应补上 `ch5..ch8` 的条目，否则第二个 tab 的格子显示 `-`。

### 验证
离屏渲染截图（`QT_QPA_PLATFORM=offscreen` + `QWidget.grab()`）确认：
- 帧数表：CH1–CH4 的 DAQ / Process / Storage 三列数值齐全且次序正确
  （DAQ ≥ Process ≥ Storage）
- 特征表：7 个字段 × 4 通道全部有值，行列对齐
- 4 个波形图都有曲线，横坐标 0–511（NORD=512），曲线铺满整个宽度
- 宏替换后 30 个通道地址逐个 `caget`，读不到者 0 个

注：`frame: -1` 是 `ModuleProcess::frameIndex_` 在 sim 下的既有取值，
`main_webui` 的 `/api/status` 同样是 -1，两边一致，非桥接问题。

## 补充：波形毛刺（2026-11-13）

现象：波形不是递增斜坡，中间大量跳变到 6 万多。

### 先排除“数制”问题
base64 载荷是 **uint16 主机序（LE）**，桥与 webui 的 `base64ToUint16LE` 一致，
两边数值本来就应当相同。用同一串 base64 交叉验证 Python 与 C++ 结论一致后，
问题才落到下面两处。

### 根因 1：桥里的 base64 解码器位移写错（我写的）
```cpp
out.push_back((val >> (bits - 8)) & 0xFF);   // bits=4 时是移位 -4，UB
```
标准实现是 `val >> bits`。同一串 base64：

| 解码器 | 前 16 字节 |
|---|---|
| Python `base64.b64decode` | `00 00 80 00 00 01 80 01 …`（正确） |
| 修前 C++ | `00 00 00 00 00 80 00 00 …`（整体错位） |

已改为 `val >> bits`，并加注释说明为什么不能写 `bits - 8`。

### 根因 2：sim 的波形是“字节递增”，不是 16 位ramp
`module_simsource::buildFrame` 原来按字节写 `(w+1)&0xFF`。两个字节拼成 16 位数后
每 128 个采样就回绕一次（65280 → 258），画出来必然是锯齿。

改为按 16 位大端写递增斜坡（与 `frame_parser` 的 `readU16BE` 一致）：
```cpp
uint16_t v = (uint16_t)(i * 128);   // 0 .. 65408
```
注：`min/max/sum_l/sum_h/vpp` 取的是帧尾 trailer 里的字段（`calibrate()`），
不来自波形采样，所以这几个值不会随波形改变。

### 根因 3（运维）：同前缀跑了多个 softIoc
现场一度有 3 个 `softIoc` 同时广播，CA 客户端随机连其中一个，导致同一 PV
时而读到 0、时而读到正常值。`run_epics.sh` 已加单实例保护：
起 IOC 前先清掉残留的 `softIoc` / `msm_epics`；
若 IOC 由外部独立管理，设 `MSM_NO_IOC=1` 跳过这一段。

### 验证
```
ch1..ch4: len=512  首个=0  末个=65408  严格递增=True
```
离屏截图：四路波形均为一条从 0 到 65408 的直线，横坐标 0–511。




