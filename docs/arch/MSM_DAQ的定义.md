# MSM_DAQ 帧定义

## 帧类型

| 帧名 | 触发方式 | 功能 |
|------|----------|------|
| **Init** | 进程启动时 main 发送；start 链重配时再次发送 | **编排入口：顺序执行 ReadConfig → InitHW → UpdateConfig** |
| ReadConfig | Init 内部调用 / 运行时单独调用 | 从 key-value 加载到 DAQ_Config 内存容器（不操作硬件） |
| InitHW | Init 内部调用 / 运行时单独调用 | SDK 扫描 → open+init 全部板卡（首次）；hw 已就绪时单独调用则先 close 再重新 open+init 以重置硬件 |
| UpdateConfig | Init 内部调用 / 运行时单独调用 | **单点：全部硬件寄存器写入（板卡级 + 通道级）** |
| Start | 用户触发 | 启动接收 → Settle → 3010 TRIG=ON |
| ReadOnce | RunLoop 空闲时自投递 | 非阻塞读帧 → ringbuffer |
| Stop | 用户触发 | 3010 TRIG=OFF → flush 残留 → 停止通道（不 close 板卡） |
| CloseHW | 进程退出时 | close 全部板卡 → SDK 反初始化 |

---

## Init 帧（启动编排入口）

**触发**: 进程启动时 main 发送 `Init >> ->| DAQModule`（携带完整配置 key-value）；
start 链重配时（每次 Start 前）再次发送。

**流程**（顺序执行，不投递消息，直接调用）:
1. `onReadConfig(msg.data)` — 加载配置到内存容器
2. `initHW()` — SDK 扫描、open+init 板卡（仅 hw 未就绪时执行；已就绪则跳过，不关闭重开）
3. `updateConfig()` — 板卡级 + 通道级寄存器全部写入硬件

> Init 是三帧的显式编排者，三帧之间不存在链式耦合。ReadConfig / InitHW / UpdateConfig 保持各自独立可调用。
> 重复 Init（hw 已就绪）只刷新内存配置并重新推参，不动硬件连接——这是 start 链
> 「每次 Start 重配」的基础（task 017）。

---

## ReadConfig 帧

**触发**: Init 内部调用；或运行时单独发送 `ReadConfig >> ->| DAQModule`（兼容旧名 `Configure`）。

**流程**:
1. 接收 key-value 参数表（由 `distributeConfig()` 从 JSON 文件生成）
2. 解析到 `DAQ_Config` 容器（card 列表、channel 配置、TrigSource/TrigLevel 等）
3. 根据 `runEnable` 通道数量创建对应数量的 **ringbuffer**（256MB/通道）
4. 置 `configured_ = true`

> ReadConfig 不做任何硬件操作，只负责内存数据结构和 ringbuffer 的建立。

---

## UpdateConfig 帧

**触发**: Init 帧内部调用（`①ReadConfig → ②InitHW → ③UpdateConfig`）；或运行时单独发送 `UpdateConfig >> ->| DAQModule`（如 start 链重配后重新写参）。

**功能**: **单点** —— 所有硬件配置（板卡级 + 通道级）统一在此完成。

**流程**:
1. 校验 `hwReady_ == true`，否则跳过
2. **板卡级参数**（每卡一次，不依赖配置容器 slot 映射）:
   - 1073: `ClockSource=0` → `SyncWindowEnable=0` → `SyncSource=0`
   - 3010: `SyncTrigTrigMux=2` → `SyncTrigGlbSync=1` → `TrigSource=2` → `SoftwareSync=1`
3. **通道级参数**: 遍历 `DAQ_Config.cards`，按 slot 匹配已 open 的 1073 卡
   - 逐通道: `GlobalTrigEnable=1`, `TrigSource`, `TrigMode`, `TrigLength`, `TrigPulseWidth`, `VoltageRange`, `TrigLevel`
4. 填充 `activeChannels_[]`（通道 → cardHandle + ring 映射）
5. 若 0 个 active 通道则报错

> **设计约束**: 这是唯一一处写硬件寄存器的代码（InitHW 仅 open+init，不写参数）。ReadConfig 改内存 → UpdateConfig 推硬件，两者解耦。

---

## InitHW 帧

**触发**: Init 帧内部调用；或运行时单独发送 `InitHW >> ->| DAQModule`。

**功能**: 卡片级初始化（自检、校准等慢操作）。首次正常执行；hw 已就绪时先 close 全部板卡再重新 open+init（SDK 不反初始化），以强制硬件重置。

**流程**:
1. **首次执行**: `CxDeviceSdkInit()` — SDK 初始化
   **重复执行**（hw 已就绪）: 跳过 SDK 初始化，先 close 全部已打开板卡（card3010_ + cards1073_），清除卡片列表
2. 枚举 PXIe 机箱，发现所有板卡
3. 分类收集 **1073** 采集卡和 **3010** 触发卡
4. 按 slot 号排序 1073 卡
5. 逐张 1073 卡: `CxDeviceSetCard("open")` → `CxDeviceSetCard("init")`
6. 3010 卡: `CxDeviceSetCard("open")` → `CxDeviceSetCard("init")`
7. 置 `hwReady_ = true`

> InitHW 只做 SDK 级别初始化（open + init 自检校准），**不写任何寄存器参数**。参数配置在 UpdateConfig 帧完成。
> InitHW 完成后板卡处于 **open → init 但未配置** 状态。

---

## Config 帧

**触发**: Init 后立即执行一次；运行中可单独调用。

**功能**: 接受 NI string 格式的逐项参数覆盖，修改 `DAQ_Config` 容器内容。支持运行时修改 TrigSource、TrigLevel 等参数（下次 Start 时生效）。

**不操作硬件**。硬件配置统一在 Start 帧完成。

---

## Start 帧

**触发**: 用户发送 `Start >> ->| DAQModule`。

**前置条件**: 
- ReadConfig 已完成（`configured_ == true`）
- InitHW 已完成（`hwReady_ == true`）
- UpdateConfig 已执行（`activeChannels_[]` 已填充）

**流程**（不涉及 config 推硬件，仅启停）:
1. CAS 置 `acquiring_ = true`，防止重入
2. 启动所有 active 通道接收:
   - `SetChannelRecvBufferSize(16MB)` → `StartChannelRecv()` → `SetChannel("run")`
3. **Settling 延时 2s** — 等待 1073 采集稳定
4. `CxDeviceWriteReg(0x1C020, 1)` — 3010 **触发输出 ON**
5. 进入采集状态

> 3010 触发输出在 1073 采集启动 + 2s 稳定之后才使能，确保第一帧触发时 1073 已准备好接收，从硬件层面保证帧对齐。
> 通道参数配置由 UpdateConfig 帧提前完成，Start 只负责启停。

---

## ReadOnce 帧

**触发**: RunLoop 空闲时自投递（`onIdle` → `post("ReadOnce")`），每 10ms 一次。

**前置条件**: `acquiring_ == true`。

**流程**:
1. 遍历所有 active 通道
2. 对每通道调用 `CxDeviceReadChannelFrames()`（非阻塞，立即返回）
3. 若 `n > 0`:
   - 计算 `bytes = n × flen`
   - `memcpy` 逐帧搬运到临时 buffer
   - `ring->writeFromDevice()` 写入 ringbuffer
   - 更新 `totalFrames` 计数
4. 若 `n == 0`: 跳过，等待下一次 ReadOnce

> ReadOnce 只负责搬运原始帧至 ringbuffer，不做解析。解析由 ModuleProcessFull 从 ringbuffer 中独立 peek。

--- 

## Stop 帧

**触发**: 用户发送 `Stop >> ->| DAQModule`。

**流程**（不涉及 close 板卡，仅停止采集状态）:
1. CAS 置 `acquiring_ = false`
2. `CxDeviceWriteReg(0x1C020, 0)` — 3010 **触发输出 OFF**（停止产生新触发）
3. `sleep_for(200ms)` — 等待板卡 FIFO 稳定
4. `flushAll()` — 遍历所有通道，最后一次 `CxDeviceReadChannelFrames()`，将残留数据写入 ringbuffer
5. `stopChannels()` — 逐通道 `StopChannelRecv()` → `SetChannel("stop")`

> 3010 触发先 OFF，再 flush 1073 残留，确保停止前最后一帧数据不丢失。
> 板卡保持 open/init 状态，可立即再次 Start。

---

## Exit 帧

**触发**: 进程退出（SIGINT/SIGTERM 或 `Shutdown` 消息）。

**逻辑判断**:
1. 若 `acquiring_ == true` → 执行 **Stop**（停止采集，不 close 卡）
2. 发送 **CloseHW** → 关闭所有板卡、SDK 反初始化

## CloseHW 帧

**触发**: 进程退出时由 main 发送 `CloseHW >> ->| DAQModule`。

**流程**:
1. 3010: `CxDeviceSetCard(card3010_, "close")` → `card3010_ = nullptr`
2. 所有 1073: `CxDeviceSetCard("close")`
3. `CxDeviceSdkDeinit()`
4. 置 `hwReady_ = false`

> CloseHW 只在进程退出时执行一次，板卡生命周期终结。

---

## 帧序列示意图

```
┌─────────────────────────────────────────────────────┐
│ main                                                │
│  distributeConfig(isInit=true)                       │
│       │                                             │
│       ▼ (Init 消息)                                  │
│  ┌──────────────────────────────┐                    │
│  │ Init 帧 (编排入口)            │                    │
│  │  ① onReadConfig(msg.data)    │                    │
│  │  ② initHW()                  │                    │
│  │  ③ updateConfig()            │                    │
│  └──────────┬───────────────────┘                    │
│             ▼                                       │
│           Idle                                      │
│             │                                       │
│       send("Start")                                 │
│             │                                       │
│             ▼                                       │
│         Start → RunLoop                              │
│             │                                       │
│       send("Stop")                                  │
│             │                                       │
│             ▼                                       │
│          Stop → Idle                                 │
│             │                                       │
│       send("CloseHW")                               │
│             ▼                                       │
│         CloseHW                                      │
└─────────────────────────────────────────────────────┘

详细帧序列：

Init（编排入口）
   ├── ① ReadConfig ── 加载 JSON → DAQ_Config → ringbuffer（内存）
   ├── ② InitHW ─── SDK_Scan → open+init 1073+3010（不写参数）
   └── ③ UpdateConfig ── 板卡级参数 + 通道级寄存器 → activeChannels_[]
         │
         ▼
       Idle  ◄───────────────────────────┐
         │                              │
    Start 触发                          │
         │                              │
         ▼                              │
    Start ── StartChannelRecv → 2s Settling → 3010 TRIG=ON
         │                              │
         ▼                              │
    RunLoop ─── ReadOnce ───────────────┤  (onIdle 自投递，10ms)
         │                              │
    Stop 触发                           │
         │                              │
         ▼                              │
    Stop ── 3010 TRIG=OFF → 200ms → flushAll → stopChannels
         │                              │
         ▼                              │
    Idle  ──────────────────────────────┘
         │
    Exit 触发
         │
         ▼
    Stop (若 running)
         │
         ▼
    CloseHW ── close 全部板卡 → SDK_Deinit
```

## 关键设计原则

1. **Init 帧是显式编排者**。ReadConfig / InitHW / UpdateConfig 三帧无链式耦合，由 Init 帧按 `①→②→③` 顺序直接调用。任一帧均可独立发送（`ReadConfig` 重读配置、`UpdateConfig` 运行时改参、`InitHW` 诊断重初始化）。

2. **分层生命周期**:

   | 阶段 | 操作 | 写入硬件? | 频次 |
   |------|------|-----------|------|
   | **Init** (编排) | 顺序调用 ReadConfig + InitHW + UpdateConfig | 见子帧 | 启动一次 / 每次 Start 重配 |
   | ReadConfig | 加载 JSON → DAQ_Config → ringbuffer | 否（纯内存） | 一次 / 每次 Start 重配 |
   | InitHW | SDK init → 扫描 → open+init 板卡 | 仅 open+init（不自检校准写参） | 仅 hw 未就绪；重复 Init 跳过 |
   | UpdateConfig | 板卡级 + 通道级寄存器写参 | **是（唯一入口）** | 按需（首次 / 每次重配） |
   | Start/Stop | 启停采集、TRIG ON/OFF | 仅 TRIG 寄存器 | 多次循环 |
   | CloseHW | close 板卡 → SDK_Deinit | 仅 close | 一次（退出） |

3. **UpdateConfig 是唯一写硬件寄存器的位置**。InitHW 只做 `open` + `init`（SDK 级自检校准），不写参数。参数寄存器由 UpdateConfig 统一写入。

4. **3010 与 1073 同进程管理**，InitHW 一同 open/init，CloseHW 一同 close。

5. **3010 触发输出晚于 1073 采集启动**（Start: 启动接收 → 2s settling → 3010 TRIG=ON），从硬件源头保证帧号对齐。

6. **Stop 帧先断触发再 flush**（3010 TRIG=OFF → 200ms → flushAll → stopChannels），板卡保持 open，可立即再次 Start。

7. **ReadOnce 非阻塞**，使用 `CxDeviceReadChannelFrames()` 直接读取 SDK 内部 buffer。

8. **RunLoop 自投递 ReadOnce**，10ms 周期，不阻塞主消息循环。
