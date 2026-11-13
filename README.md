# MSM

**面向 AI 编码的模块化数据采集框架。** 以消息作为唯一的跨模块接口，使系统级改动在上下文、影响面、验证成本三个维度上可控。

## 论证链

| AI 写系统级代码的失效 | 对应的结构约束 | 得到的性质 |
|:--|:--|:--|
| 上下文装不下调用链 | 模块即上下文单元：独立线程、消息队列、状态机，编译期与兄弟模块隔离 | 可审查，改动边界即模块边界 |
| 隐式耦合使影响面不可枚举 | 消息是唯一的跨模块接口，无共享全局状态 | 可定位，影响面等于契约面 |
| 正确性只能依靠整机验证 | 模块可独立实例化、以消息驱动，硬件可被仿真源替换 | 可测试，单个模块即可运行验证 |

三条约束收敛到同一结论：任一改动的边界，等于一个模块加一份消息契约。

![MSM 监控界面](docs/images/webui.png)

*监控界面：帧计数、特征表、四通道波形实时刷新，数据来自仿真源。*

## 约束的落地形态

### 模块自治

每个模块持有独立线程、消息队列与状态机，头文件不引用兄弟模块。模块边界与编译边界重合，改动的工作集即该模块源码，评审单位即该模块。

### 消息即契约

跨模块只存在 `Message` 一种通信原语：33 条命令、28 个对外变量声明。契约面可枚举、可评审、可静态检查。IPC、REST、EPICS 等接口层由变量声明自动导出，不构成额外契约。

### 进程级可验证

模块可脱离系统独立实例化，以消息序列驱动状态迁移；仿真源模块可替换采集硬件，验证不依赖整机与仪器排期。

## 目录结构

框架核心只有一个目录，其余全部是可替换的实例。换一套硬件、换一类业务，核心代码不动。

| 类别 | 目录 | 职责 |
|:--|:--|:--|
| **框架核心** | `module_core/` | 唯一的框架代码：消息模型与 MessageBus、Module 基类、VarTable，以及 IPC、REST、EPICS 接口层 |
| 实例：业务模块 | `module_daq/` | 采集实例，PXIe 1073 + 3010 硬件控制 |
| | `module_process/` | 帧解析与特征提取（Vpp、X、Y、光强、波形） |
| | `module_storage/` | 批量落盘与文件轮转 |
| | `module_config/` | 配置实例，负责 pending 到 runtime 的交接 |
| | `module_simsource/` | 仿真数据源，替代采集卡 |
| 实例：启动入口 | `main_cli/` | CLI 入口，内建 IPC server |
| | `main_webui/` | 浏览器监控入口，IPC 转 HTTP 与 SSE，ECharts 绘图 |
| | `main_epics/` | EPICS CA 桥与 PyDM 控制界面 |
| 实例：配置与依赖 | `config/` | 共享配置，`pv_map.json` 是 PV 的唯一真源 |
| | `sdk_include/` | 硬件 SDK 头文件 |
| 文档 | `docs/`、`tasks/` | 设计文档与可追溯的任务流水 |

`module_calculator/`、`module_datachannel/`、`main_config_editor/`、`tools/` 同样是实例，按需取用。

## 用法

构建：`cmake -B build && cmake --build build -j`

| 入口 | 用途 |
|:--|:--|
| `./main_cli/build/msm_cli_simsrc` | 仿真采集，4 通道，不依赖硬件 |
| `./main_cli/build/msm_cli_hwdaq` | 真实硬件，1 GSa/s |
| `./main_webui/build/msm_webui` | 浏览器端波形与状态监控 |
| `./main_epics/run_epics.sh` | 启动 IOC 与 CA 桥 |
| `./main_epics/run_pydm.sh` | 启动 PyDM 控制界面 |

CLI 命令：`start`、`stop`、`enablesave`、`disablesave`、`status`、`quit`。

配置变更写入 `config/app_pending_config.json`，于下一次 `start` 时由后端完成 pending 到 runtime 的交接并重配硬件，无需 reload。

新增模块：继承 `Module`，实现 `handleState()`，以 `declareVar()` 声明对外变量。声明即导出，IPC、REST、EPICS 三个入口自动可访问。

## 设计文档

| 文档 | 内容 |
|:--|:--|
| `docs/arch/design.md` | 框架设计：消息模型、MessageBus、接口层 |
| `tasks/` | 29 份决策与实测记录 |
| `docs/arch/MSMvsEPICS.md` | 与 EPICS、Bluesky 的架构对比 |

---

消息语法沿用 NI CSM 的约定，对 LabVIEW 技术栈无迁移成本。EPICS 桥、config CLI、PyDM 前端均为后加能力，未改动既有消息契约，以新增订阅方接入。当前为单机部署，多机调度与硬实时指标未覆盖。
