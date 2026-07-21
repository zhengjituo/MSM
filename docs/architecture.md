# MSM_DAQ 架构设计

## 核心机制

### 消息驱动状态机
- 所有模块继承 `msm::Module`，通过 `handleState(Message)` 处理消息
- 模块间不直接 import/header 依赖，只通过模块名字符串寻址
- 消息通过 `MessageBus` 路由

### 帧定义
参见各模块设计文档中的"状态帧说明"章节。

每帧是一个独立的消息处理入口，模块在 `handleState()` 中按 `msg.cmd` 分发。

## 数据流

```
SimDAQ / ModuleDAQ（产生帧）
    → ring buffer（SPSC lock-free）
    → ModuleProcess / ModuleProcessFull（peek + 解析）
    → SaveBatchAll MSG
    → ModuleStorage（fwrite 存盘）
```

硬件模式下：
```
3010 触发卡（时序控制）
  └→ 1073 采集卡（4 通道）→ ring buffer → Process → Storage
```

## 模块一览

| 模块 | 目录 | 功能 |
|------|------|------|
| 框架核心 | `module_core/` | MessageBus、Module 基类、IPCServer 等 |
| 采集 | `module_daq/` | 1073 + 3010 硬件控制、配置加载 |
| 处理（轻量） | `module_process/` | peek 首帧 TrigID/BunchID |
| 处理（全帧） | `module_process_full/` | 全帧提取 + 跨通道对齐检查 |
| 存盘 | `module_storage/` | fwrite 自动 rotate |
| 仿真 | `module_sim/` | 虚拟帧发生器 |
| main v1 | `module_main_v1/` | 仿真模式入口 |
| main v2 | `module_main_v2/` | 硬件全功能 display + WebUI |
| main v3 | `module_main_v3/` | 硬件采集存盘 |
