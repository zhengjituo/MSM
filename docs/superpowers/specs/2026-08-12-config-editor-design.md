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
main_config_editor/            (独立进程 msm_config_editor，端口 8081)
 ├─ CMakeLists.txt
 ├─ main_config_editor.cpp     含 httplib server + REST 接口
 └─ web/config_editor.html     两 tab 配置页面（dark 主题，与现有 webui 一致）

main_webui/main_hwdaq.cpp      (改动：config 入口按钮 + 拉起进程接口)
 └─ 主页面 toolbar 新增「⚙ 配置」按钮（DAQ 运行中置灰）
```

组件职责：
- **msm_config_editor**：唯一读写 `config/app_initial_config.json` 的服务端。
  持有所知可编辑键集合与枚举映射表（镜像 Qt 的 KWorkModeMap 等）。
- **web/config_editor.html**：纯前端展示与表单收集，通过 REST 与编辑进程交互。
- **msm_webui**：只负责入口（按钮 + 拉起进程），不参与配置文件读写。

## 3. 配置文件路径解析

`msm_config_editor` 用 `readlink("/proc/self/exe")` 定位二进制目录
`<root>/main_config_editor/build/`，向上两级得 `<root>`，配置文件路径为
`<root>/config/app_initial_config.json`。与现有 webui 解析 html 路径同法，不依赖编译宏。

## 4. REST 接口（msm_config_editor）

| 接口 | 方法 | 说明 |
|------|------|------|
| `/` | GET | 返回 `web/config_editor.html` |
| `/api/config` | GET | `ordered_json` 解析整个文件返回（键序/未知字段全保留） |
| `/api/config/save` | POST | 按作用域应用通道参数集并原子写回 |
| `/api/config/save-app` | POST | 合并 `app_config` 字段并原子写回 |

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
- `params`：前端表单收集的可编辑通道参数（不含 `cardIndex/channel/slot` 身份键，
  含 `runEnable` 布尔）。
- 服务端校验：`params` 的键必须属于已知可编辑键集合（对齐 Qt `ChannelConfig`
  字段 + `DownSample`/采样率），超出即拒绝，防止注入未知键。
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
  - 「二次公式参数」组（K/Kx/Ky/OffsetX/OffsetY/K1st/B1st）：所有通道均显示
    （config 文件本就每通道都有这些键；Qt 只在 Channel 1 分组展示，界面差异不影响文件）
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

## 6. 入口保护（msm_webui）

- 主页面 `msm_daq_hw.html` toolbar 新增「⚙ 配置」按钮。
- 客户端：按 DAQ 状态（现有 `g_snap.running` 轮询/SSE）置灰按钮。
- 服务端：新增 `POST /api/config-open`，内部再校验 `g_snap.running`，
  运行中返回拒绝；未运行则 fork/exec 拉起 `msm_config_editor`（复用现有
  `launchBackend` 的 fork/exec 模式），返回进程 pid + 端口，前端新标签页打开
  `http://localhost:8081`。若进程已运行则直接返回已运行状态。
- 退出：config 编辑器是独立进程，关闭浏览器不影响它；提供「退出」按钮或
  由用户 `Ctrl+C`/关闭终端退出（与 msm_webui 相同的优雅退出路径）。
- 说明：按解耦原则，config 编辑器进程**自身不做** DAQ 运行态门控（它不感知
  DAQ）；入口保护（按钮置灰 + `/api/config-open` 校验）是唯一的控制点。用户若
  手动启动编辑进程并在采集中改配置，改动同样只在**下一次 Start** 生效（运行时
  DAQ 使用的是启动时快照，且原子写保证不会读到半个文件）。

## 7. 错误处理

- 文件不存在/JSON 解析失败：`GET /api/config` 返回 500 + 错误信息，
  页面提示「配置文件缺失或损坏」。
- 保存时参数校验失败：返回 400 + 具体键名。
- 保存写文件失败：返回 500，保留原文件。
- DAQ 采集中点 config 按钮：按钮置灰 + toast 提示「请先停止采集」。
- 8081 端口被占：拉起失败时 webui 返回错误；用户可手动启动编辑进程。

## 8. 测试

- **往返兼容性**：读取现有 `config/app_initial_config.json` → 不做修改直接写回 →
  与原文逐字节/逐键对比（键序、未知字段、值类型不变）。
- **作用域保存**：分别用 channel/card/chassis 保存，断言只改目标通道、身份键不变、
  未知键保留。
- **入口保护**：DAQ 运行态 vs 停止态，config 按钮与 `/api/config-open` 行为。
- **枚举校验**：非法枚举值（不在映射表内）保存被拒。
- 手动：启动 msm_config_editor → 浏览器编辑 → 保存 → 重启 DAQ 验证参数生效。

## 9. 文件清单

| 文件 | 动作 |
|------|------|
| `main_config_editor/CMakeLists.txt` | 新增 |
| `main_config_editor/main_config_editor.cpp` | 新增 |
| `main_config_editor/web/config_editor.html` | 新增 |
| `main_webui/main_hwdaq.cpp` | 修改（config 按钮 + /api/config-open） |
| `main_webui/web/msm_daq_hw.html` | 修改（toolbar 加按钮 + JS） |
| 根 `CMakeLists.txt` | 修改（add_subdirectory） |
