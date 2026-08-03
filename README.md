# MSM — C++ 实现的可通信状态机框架 + 高速数据采集实例

**把 NI CSM 的理念带进 C++**：基于消息的模块化状态机框架，支持同步调用、异步消息、广播订阅，天然适配自动化测试（ATE）与高速数据采集场景。参考 [NEVSTOP-LAB/Communicable-State-Machine](https://github.com/NEVSTOP-LAB/Communicable-State-Machine) 设计思想。

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)]() [![Platform](https://img.shields.io/badge/Platform-Linux-lightgrey)]() [![EPICS](https://img.shields.io/badge/EPICS-PVA-green)]() [![License](https://img.shields.io/badge/License-MIT-green)]()

---

## 为什么值得一看

- **熟悉 CSM？语法零门槛** — 沿用 NI CSM 消息字符串语法（`-@` 同步 / `->` 异步 / `->|` 无返回 / `<broadcast>` 广播 / `<register>` 订阅），LabVIEW 用户可无缝迁移。
- **模块化状态机** — 每个模块独立线程 + 消息队列 + 状态机，模块间仅靠消息寻址，**不互相依赖头文件**，可独立启动、独立调试。
- **一个框架，五套接口** — 同一套变量自动暴露给 IPC / REST / EPICS PVA / ZMQ / WebUI，新增模块零配置接入。
- **不只是一套框架** — 自带一个真实可跑的**高速采集系统实例**（PXIe 1073 + 3010，1 GSa/s，4 通道），高频数据走零拷贝 ringbuffer 闭环，不阻塞消息总线。
- **面向 ATE 扩展** — 消息语法与执行模型天然契合 TestStand 式脚本自动化，可作为未来 ATE 测试框架的引擎底座。

```
┌────────────────────────────────────────────────────────────┐
│                      MSM_Core（框架核心）                   │
│   消息驱动状态机 · 同步/异步/广播订阅 · 模块化独立调试        │
│  MessageBus │ Module │ VarTable │ NIStringParser │ DataChnl │
│  ──────────────────────────────────────────────────────     │
│  接口层：IPC │ REST │ EPICS PVA │ ZMQ Gateway │ WebUI       │
├────────────────────────────────────────────────────────────┤
│   数据采集实例：DAQ → ringbuffer → Process → Storage         │
└────────────────────────────────────────────────────────────┘
```

---

## 消息机制（与 CSM 一脉相承）

模块之间唯一的通信载体是 `Message`（命令 + 数据），支持 NI 字符串语法直接书写：

```
Calculate >> x=10,y=20 -@ SubModule          // 同步调用：阻塞等待返回
Configure >> mode=vpp -> SubModule           // 异步有返回
Notify >> event=ready ->| SubModule          // 异步无返回
Status >> ch1=1.23 -><broadcast>             // 广播：所有订阅者
Status@Process >> handle -><register>        // 订阅 / 取消订阅
```

由 `MessageBus`（中央邮局）统一路由，四种模式底层实现：

| 模式 | 符号 | 实现机制 | 典型用途 |
|------|------|----------|----------|
| 同步调用 | `-@` | `sendAndWait()` + correlationId + promise | 业务流程编排、安全等待 |
| 异步有返回 | `->` | 异步响应按 correlationId 回传 | 后台任务 |
| 异步无返回 | `->\|` | 直接投递到目标消息队列 | 事件通知 |
| 广播/订阅 | `<broadcast>`/`<register>` | 订阅表分发 + 跨进程 PUB | 状态变更推送 |

路由规则：目标为 `<broadcast>` → 分发订阅者；带响应 → 唤醒等待方；带 `_remote` → 走 MSMGateway 跨进程；否则按模块名投递。

## 状态机轮转机制

每个模块继承 `msm::Module`，拥有**独立线程 + 消息队列**：

```cpp
Module::runLoop() {
    while (running_) {
        cv_.wait_for(50ms, /* 消息到来即时唤醒 */);
        if (!queue_.empty()) handleState(msg);   // 按 cmd 分发
        else onIdle();                            // 空闲帧：自投递/轮询
    }
}
```

- 消息驱动 + `notify_one()` 即时唤醒，不做忙轮询
- 每 100 条消息强制一次 idle，保证空闲帧不被饿死
- `start()`/`stop()` 显式生命周期，优雅退出

## 模块化独立调试

模块通过 `VarTable` 声明变量，接口层自动发现，**新增模块零配置**：

```cpp
declareVar("ch1_frames", ch1Frames_);          // 自动类型推断
declareVar("trig_level", trigLevel_, "io");    // i=可写 / o=只读 / io
dumpVarsJSON("msm_vars.json");                 // schema 导出，接口层自动映射
```

## 五套接口，一套变量

| 接口 | 协议 | 粒度 | 跨机器 | 广播 | 适用 |
|------|------|------|:---:|:---:|------|
| **IPC** | UNIX socket | 变量 | ❌ | ❌ | 本地调试、CLI |
| **REST** | HTTP | 变量+消息 | ✅ | ❌ | Web / 第三方集成 |
| **EPICS PVA** | PVA / softIOC | 变量 | ✅ | ✅ | EPICS 生态（caget/caput） |
| **ZMQ** | REP+DEALER+PUB+SUB | 消息 | ✅ | ✅ | 多进程 / 多机互连 |
| **WebUI** | IPC↔HTTP + SSE | 状态帧 | ✅ | ✅ | 浏览器实时监控（ECharts 波形） |

---

## 数据采集实例（真实可跑）

一套完整的**高速采集系统**（PXIe 1073 采集卡 + 3010 触发卡，1 GSa/s，4 通道），展示模块化拆分与数据流转。

### 模块划分

| 模块 | 职责 |
|------|------|
| `module_core` | 框架核心：MessageBus、Module 基类、接口层 |
| `module_config` | 配置唯一来源，JSON 加载 + 分发 |
| `module_daq` | 1073+3010 硬件控制，读帧 → ringbuffer |
| `module_process` | 帧解析 + 特征提取（VPP/X/Y/光强）+ 波形 |
| `module_storage` | 批量落盘 + 自动文件 rotate |
| `module_simsource` | 虚拟帧发生器（无硬件开发调试） |
| `main_cli` / `main_webui` | CLI+IPC 入口 / Web 监控入口 |

### 数据流转

```
3010 触发卡 → 1073 采集卡(4ch)
   └→ ModuleDAQ（非阻塞读帧）
        └→ ringbuffer（SPSC lock-free，256MB/通道）
             ├→ ModuleProcess：解析 → 特征值/波形 → IPC/REST/WebUI
             └→ ModuleStorage：批量落盘
```

**关键取舍**：高频原始数据在模块内闭环（零拷贝 ringbuffer），低频状态/特征走消息总线（广播），各模块互不阻塞；仿真源可无缝替换硬件采集，同一套 UI 无需改动。

---

## 构建与运行

```bash
cmake -B build && cmake --build build -j

./main_cli/build/msm_cli_hwdaq    # 硬件采集（含 IPC shell）
./main_cli/build/msm_cli_simsrc   # 仿真采集（无需硬件）
./main_webui/build/msm_webui      # Web 监控

# CLI: start / stop / enablesave / disablesave / status / reload / quit
```

## 目录结构

```
MSM/
├── module_core/        # 框架核心（MessageBus、Module、VarTable、接口层）
├── module_config/      # 配置模块
├── module_daq/         # 采集模块（1073 + 3010）
├── module_process/     # 处理模块（帧解析/特征提取）
├── module_storage/     # 存盘模块
├── module_simsource/   # 仿真数据源
├── main_cli/           # CLI + IPC 入口
├── main_webui/         # WebUI 入口（IPC↔HTTP + SSE + ECharts）
├── config/             # 共享配置
├── sdk_include/        # 硬件 SDK
├── docs/               # 设计文档
└── tasks/              # 可追溯的开发任务流水
```

## 文档

- `docs/arch/design.md` — MSM 框架完整设计（消息、NI 语法、MessageBus、接口层、DataChannel）
- `docs/arch/MSM_DAQ的定义.md` — 采集实例帧生命周期与硬件编排
- `docs/arch/MSMvsEPICS.md` — 与 EPICS / Bluesky 框架对比
- `docs/modules/` — 各模块设计说明
- `docs/data_formats/frame_format_PD.md` — 物理帧结构
