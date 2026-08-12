# Config Editor 设计 — 独立 web 配置编辑器（对齐 Qt 参考实现）

日期：2026-08-12
状态：设计中

## 1. 背景与目标

### 目标
在 MSM_DAQ 项目中提供一个**独立进程**的 web 配置编辑页，读取/展示/修改/保存
`config/app_initial_config.json`，界面与 Qt 参考实现
`qtProgram/config_management/frame_software_parameter` 对齐。与原业务（DAQ 采集）
完全解耦：业务只读配置文件，编辑通过独立 web 页完成。

### 硬性约束
- **JSON 文件格式不变、与现有完全兼容**：编辑 `app_initial_config.json` 时保留
  所有键、键顺序、未知字段、值类型。写回后业务自写解析器
  （`module_config/config_impl.cpp` 的 `findValue/findBrace`）必须无感。
- **入口保护**：原 webui 主页面的 config 按钮在 DAQ 采集中置灰、不可点击、
  不拉起配置进程（对齐 Qt `MainWindow.cpp:368-371` 的 `m_isRun` 门控）。配置改动
  仅在**下一次 Start** 生效。
- **解耦**：config 编辑器进程不依赖 DAQ IPC、不依赖 webui 运行状态；可独立启动使用。

### 后续工作（本次不做）
- 变采样率已生效（固件支持），采样率（`DownSample`）决定波形每个点的时间间隔。
  本次只把参数界面做对齐；**波形时间轴按 sample_rate 重算**留待后续任务。

## 2. 架构与组件

```
module_config_editor/            配置编辑业务核心 ConfigEditorModule : msm::Module（挂 MessageBus）
 ├─ 持有 ordered_json 配置、已知可编辑键集合、acquiring_ 锁
 ├─ handleState: GetConfig / SaveChannel / SaveCard / SaveChassis / SaveApp / GetLock
 └─ 周期探测 DAQ 采集状态（只读 IPC Status 查询）→ acquiring_

main_config_editor/              独立进程 msm_config_editor（端口 8081）
 ├─ CMakeLists.txt
 ├─ main_config_editor.cpp       薄 main：MessageBus + 注册模块 + httplib（handler → sendAndWait）
 └─ web/config_editor.html       两 tab 配置页面（dark 主题，与现有 webui 一致）

main_webui/main_hwdaq.cpp        (改动：config 入口按钮 + 拉起进程接口)
 └─ 主页面 toolbar 新增「⚙ 配置」按钮（DAQ 运行中置灰）
```

组件职责：
- **ConfigEditorModule**：唯一读写 `config/app_initial_config.json` 的业务核心。
  持有所知可编辑键集合与枚举映射表（镜像 Qt 的 KWorkModeMap 等）；`handleState`
  处理保存/查询消息（`sendAndWait` + `notifyResponse` 模式，同 ModuleDAQ）；
  周期探测 DAQ 采集状态更新 `acquiring_`，采集中拒绝保存。
- **main_config_editor**：装配层：创建 MessageBus、注册/启动 ConfigEditorModule、
  加载配置；httplib server 的 handler 通过 `sendAndWait` 同步调用模块。
- **web/config_editor.html**：纯前端展示与表单收集，通过 REST 与编辑进程交互。
- **msm_webui**：只负责入口（按钮 + 拉起进程），不参与配置文件读写。

### msm_core 依据
- 同步请求/应答：`MessageBus::sendAndWait` + `notifyResponse`（ModuleDAQ Start/Status
  同模式，module_daq.cpp:98-114）。
- `Message.data` 为 string 键值表，足以承载 ~28 个通道参数（除 `runEnable` 布尔外全为
  字符串；`runEnable` 以 `"true"/"false"` 传）。
- 模块生命周期 + `onIdle` 天然承载 DAQ 状态轮询；main_cli 已将 ConfigModule 挂总线，
  配置编辑器作为独立模块保持架构一致。

## 3. 配置文件路径解析

`msm_config_editor` 用 `readlink("/proc/self/exe")` 定位二进制目录
`<root>/main_config_editor/build/`，向上两级得 `<root>`，配置文件路径为
`<root>/config/app_initial_config.json`。与现有 webui 解析 html 路径同法，不依赖编译宏。

## 4. REST 接口（msm_config_editor）

| 接口 | 方法 | 说明 |
|------|------|------|
| `/` | GET | 返回 `web/config_editor.html` |
| `/api/config` | GET | `sendAndWait` GetConfig → 返回完整配置（`config_json` 单字段携带 dump 文本，键序/未知字段全保留） |
| `/api/config/save` | POST | 按作用域应用通道参数集并原子写回 |
| `/api/config/save-app` | POST | 合并 `app_config` 字段并原子写回 |
| `/api/lock` | GET | `sendAndWait` GetLock → `{acquiring: true/false}`，前端据此实时置灰保存按钮 |

### POST /api/config/save
body：
```json
{
  "scope": "channel" | "card" | "chassis",
  "cardIndex": 1,
  "channel": 1,
  "params": { "DownSample": "500MSa/s", "TrigLength": "512", "runEnable": true, ... }
}
```
- HTTP handler 将 body 转为 `Message`（`param.*` 前缀键放入 `msg.data`，`runEnable`
  转 `"true"/"false"`），`sendAndWait` 调 ConfigEditorModule；模块应答
  `status: ok/error/locked`。
- `params`：前端表单收集的可编辑通道参数（不含 `cardIndex/channel/slot` 身份键，
  含 `runEnable` 布尔）。
- 服务端校验：`params` 的键必须属于已知可编辑键集合（对齐 Qt `ChannelConfig`
  字段 + `DownSample`/采样率），超出即拒绝，防止注入未知键。
- `acquiring_==true`（DAQ 采集中）时拒绝，返回 `status: locked`（HTTP 409）。
- 定位与写入（只覆盖 `params` 中的键，其余键原样保留）：
  - `scope=channel`：`Cards[所选卡].Channels["Channel#N"]`
  - `scope=card`：`Cards[所选卡].Channels` 全部 Channel#
  - `scope=chassis`：所有非 3010 卡的全部 Channel#（跳过 `CardIndex=3010`）
- 身份键 `cardIndex/channel/slot` 以目标通道文件中的值为准，不覆盖。

### POST /api/config/save-app
body：`{ "app_config": { "dataSavePath": "...", "runId": 147, ... } }`
- 只合并传入字段，其余保留。

### 原子写
写同目录临时文件（如 `app_initial_config.json.tmp.<pid>`）→ `fsync` →
`rename()` 原子替换。写失败保留原文件，返回错误。

## 5. 前端页面 config_editor.html

两 tab（对齐 Qt）：
- **Tab1「设备参数」Device Parameter**
  - 整机级：`ChassisNo` 展示（只读）
  - 板卡级：Card 下拉（来自文件 `Cards`，显示 `Card#N (1073, Slot m)`；
    3010 卡 `Channels:null` 无可编辑通道，通道编辑区对其禁用）
  - 通道级：Channel 下拉（来自所选卡 `Channels` 的键）+ 「通道使能」复选框
  - 通道参数网格（每键按其类型渲染）：
    - 枚举 → 下拉，**选项即 config 文件中的值**，字段名用中文标签
    - 数值（int/float，文件中为字符串）→ 文本输入 + 数值校验
    - `runEnable` → 复选框
  - 「二次公式参数」组（K/Kx/Ky/OffsetX/OffsetY/K1st/B1st）：**始终可见一组**
    （对齐 Qt `groupBox_forChannel1`），不随所选通道隐藏；保存时随作用域写入目标
    通道。旁注「每卡共用一组，仅通道1的值生效」（二次参数由 4 通道一次特征值
    计算得，运行时仅每卡通道 1 的取值生效）。
  - 3 个保存按钮（对齐 Qt 三个槽函数语义）：
    1. 保存当前通道
    2. 保存到当前板卡所有通道
    3. 保存到机箱全部 1073 板卡通道（跳过 3010）
- **Tab2「软件参数」Software Parameter**（→ `app_config`）
  - `isEpicsControlMode` 复选框、`startStorage` 复选框、
    `autoAcqInEpicsWhenSoftwareStarts` 复选框、`dataSavePath` 文本、
    `maxSaveSizeMB` 数字、`fileType` 下拉（dat/hdf5）、`fileName` 文本、
    `runId` 数字、`pvInfoUpdateFreHz` 数字（1~10）、`trigDelayTimeFix` 数字
  - 「保存」按钮

### 枚举映射表（JS 常量，镜像 Qt）
| config 键 | 中文标签 | 输入类型 | 选项（=文件值） |
|-----------|---------|---------|----------------|
| DownSample | 降采样（采样率） | 下拉 | 1GSa/s, 500MSa/s, 250MSa/s, 125MSa/s, 62.5MSa/s, 31.25MSa/s |
| TrigSource | 触发源 | 下拉 | localChannel, localBoard, global, external, force, cycle |
| TrigMode | 触发模式 | 下拉 | normal, autoMode |
| ExternalTrigPolarity | 外触发极性 | 下拉 | FallingE, RisingE |
| TrigPolarity | 触发极性 | 下拉 | Falling, Rising |
| VoltageRange | 电压量程(mV) | 下拉 | 1000, 2000 |
| WorkMode | 工作模式 | 下拉 | vpp, vmin, vmax, sum_th_h, sum_th_l |
| WaveDataEnable | 波形使能 | 下拉 | 0, 1 |
| IpGlobalRST | IP全局复位 | 下拉 | 0, 1 |
| IpLogicRST | IP逻辑复位 | 下拉 | 0, 1 |
| fileType | 文件类型 | 下拉 | dat, hdf5 |

数值字段中文标签（对齐 Qt）：采集长度(ns)、死时间(ns)、触发脉宽(ns)、触发阈值(mV)、
触发位置(ns)、触发延迟(ns)、偏置电压(mV)、有限次触发次数、高/低阈累加值求取阈值(mV)、
触发窗口开始(ns)、一次特征值修正系数 k/b、二次计算公式 K/Kx/Ky/X截距/Y截距。

## 6. 入口保护与 DAQ 门控

双层门控，均对齐 Qt「采集中不可配置」语义（`MainWindow.cpp:368-371`）：
- **入口层（msm_webui）**：主页面「⚙ 配置」按钮按 DAQ 状态（`g_snap.running`）
  置灰；服务端 `POST /api/config-open` 再校验 `g_snap.running`，运行中拒绝，
  不拉起配置进程。
- **模块层（ConfigEditorModule，更鲁棒）**：模块内周期（如 2s）探测
  `/tmp/msm_daq.sock`——发 `MSG Status >> -@ DAQModule` 并解析 `running` 字段
  （module_daq.cpp:411）；`running=true` → `acquiring_=true`，保存接口返回
  HTTP 409「请先停止采集」。覆盖「从 webui 以外别处启动 DAQ」的场景。
  探测为只读查询、不发任何命令，仍保持业务解耦。

配置改动仅在**下一次 Start** 生效（运行时 DAQ 使用启动时快照；原子写保证读不到
半个文件）。config 编辑器进程本身不依赖 webui/DAQ 才能运行，可独立启动使用。

- 拉起：`POST /api/config-open` 未运行则 fork/exec 拉起 `msm_config_editor`
  （复用现有 `launchBackend` 的 fork/exec 模式），返回进程 pid + 端口；若已运行
  直接返回已运行状态。前端新标签页打开 `http://localhost:8081`。
- 退出：config 编辑器独立进程，关闭浏览器不影响它；提供「退出」按钮或
  `Ctrl+C`/关终端退出（与 msm_webui 相同的优雅退出路径：`svr.stop()` + join）。

## 7. 错误处理

- 文件不存在/JSON 解析失败：`GET /api/config` 返回 500 + 错误信息，
  页面提示「配置文件缺失或损坏」。
- 保存时参数校验失败：返回 400 + 具体键名。
- 保存写文件失败：返回 500，保留原文件。
- DAQ 采集中点 config 按钮：按钮置灰 + toast 提示「请先停止采集」；
  已打开的 config 页面保存同样被模块层 409 拒绝并提示。
- 8081 端口被占：拉起失败时 webui 返回错误；用户可手动启动编辑进程。

## 8. 测试

- **往返兼容性**：读取现有 `config/app_initial_config.json` → 不做修改直接写回 →
  与原文逐字节/逐键对比（键序、未知字段、值类型不变）。
- **作用域保存**：分别用 channel/card/chassis 保存，断言只改目标通道、身份键不变、
  未知键保留。
- **入口保护**：DAQ 运行态 vs 停止态，config 按钮置灰与 `/api/config-open` 拒绝行为。
- **模块层门控**：mock 或真实 DAQ 运行，ConfigEditorModule 探测到 `running=true`
  后保存返回 409；停止后恢复可保存。
- **枚举校验**：非法枚举值（不在映射表内）保存被拒。
- 手动：启动 msm_config_editor → 浏览器编辑 → 保存 → 重启 DAQ 验证参数生效。

## 9. 文件清单

| 文件 | 动作 |
|------|------|
| `module_config_editor/CMakeLists.txt` | 新增 |
| `module_config_editor/config_editor_module.h` | 新增 |
| `module_config_editor/config_editor_module.cpp` | 新增 |
| `main_config_editor/CMakeLists.txt` | 新增 |
| `main_config_editor/main_config_editor.cpp` | 新增 |
| `main_config_editor/web/config_editor.html` | 新增 |
| `main_webui/main_hwdaq.cpp` | 修改（config 按钮 + /api/config-open） |
| `main_webui/web/msm_daq_hw.html` | 修改（toolbar 加按钮 + JS） |
| 根 `CMakeLists.txt` | 修改（add_subdirectory） |
