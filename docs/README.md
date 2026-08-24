# docs/ — 设计文档索引

> 阅读顺序：P1（必读）→ P2（按需）→ P3（查格式时查阅）→ P4（可忽略）

## P1 — arch/（架构核心，AI 应优先阅读）

| 文件 | 内容 | 篇幅 |
|------|------|------|
| `design.md` | MSM 框架完整设计：Message 结构、NI 语法、Module 生命周期、MessageBus 路由 | 615 行 |
| `architecture.md` | DAQ 系统架构总览：数据流 + 模块关系 | 43 行 |
| `MSM_DAQ的定义.md` | DAQ 帧架构设计：Init/ReadConfig/InitHW/UpdateConfig/Start/Stop/CloseHW 生命周期、序列图 | 268 行 |
| `MSMvsEPICS.md` | MSM 框架与 EPICS 对比 | 144 行 |
| `MSMvsEPICS_Bluesky.md` | MSM 框架与 Bluesky 对比 | 257 行 |

## P2 — modules/（各模块设计说明）

| 文件 | 内容 |
|------|------|
| `module_daq.md` | 采集模块：1073+3010 控制、帧定义 |
| `module_process.md` | 处理模块（轻量 peek 版） |
| `module_process_full.md` | 处理模块（全帧提取 + 跨通道对齐检查） |
| `module_storage.md` | 存盘模块 |

## P3 — data_formats/（数据格式 + 配置）

| 文件 | 内容 |
|------|------|
| `frame_format_PD.md` | 物理帧结构：Header/Data/Trailer、TrigID、BunchID、帧尾特征字段 |

## 当前版本状态（task 017）

- **main_cli** — 稳定。CLI + IPC 交互，支持 Storage 录盘
  - `msm_cli_hwdaq` + `msm_ipc_shell` + `msm_cli_simsrc`
  - 命令：start / stop / enablesave / disablesave / status / quit（reload 已移除）
- **配置单链式生效** — initial → pending → runtime；webui 只编辑 pending，
  后端每次 Start 自主完成 pending→runtime 交接 + 重配硬件，无运行中热改。
  设计：`superpowers/specs/2026-08-19-config-pending-chain-design.md`
- **main_webui** — 稳定。内嵌完整后端 + Web GUI
  - Config 面板编辑 `app_pending_config.json`（首启从 initial 播种）；无 reload 按钮/API
- **main_webui** — 稳定。IPC-to-HTTP 桥接 + 后端进程管理（Run/Quit）+ 特征表 + 帧索引控制
  - 通过 `MSM_CLI_PATH` 环境变量或自动推断查找 CLI 二进制
  - HW/SIM 模式选择，fork+exec 启动后端进程
  - 状态机：launching（等 IPC）→ launching（等 DAQ ready）→ running
  - SSE 实时推送 DAQ/Process/Storage 状态 + 特征表数据（4 通道 × 10 特征）
  - **`GET /api/waveform`** 返回当前帧波形原始 ADC int16（Base64 LE + 校准参数）
  - **ECharts 波形图**：2×2 网格、深色主题、dataZoom 拖拽缩放、tooltip、坐标轴
- **frame_parser** — C++ 帧解析库，独立于 module_process，偏移/算法经双向验证
  - `extractWaveformRaw()` 提取原始 uint16_t ADC 样本 + offset/scale/range

## P4 — archive/（历史/非核心）

| 文件 | 内容 |
|------|------|
| `STATUS_SNAPSHOT.md` | 2026-07-08 开发状态快照（已过时） |
| `usage.md` | 旧 MSM 框架使用指南（calculator 示例，非 DAQ 专用） |
| `superpowers/` | opencode 工作流插件设计文档 |
