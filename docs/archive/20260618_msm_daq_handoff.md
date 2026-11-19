# MSM_DAQ → module_core 升级交接文档

## 概述

已将 MSM_DAQ 的模块（DAQ/Sim/Process/Storage）从旧 `msm/` 框架迁移到新版 `module_core/`（C++17, cv 驱动, EPICS/ZMQ/HTTP 可选）。

## 本地改了什么

### module_core（通用改进）

1. **CMakeLists.txt**: 加 `option(MSM_ENABLE_EPICS/ZMQ/HTTP)`，按需编译 PVServer/MSMGateway/RESTServer
2. **IPCServer.cpp**: 加 `MSG` 命令转发（NI 字符串 → MessageBus）+ `PUT action` 100ms sleep

### module_daq（新项目）

新建 `MSM/module_daq/`，包含完整 DAQ 项目：
- `module_sim.h/cpp` — 虚拟数据源（从旧 msm 迁移，适配新 API）
- `module_daq.h/cpp` — 硬件采集（1073 + 3010，条件编译）
- `module_process.h/cpp` — peek 解析 TrigID/BunchID
- `module_storage.h/cpp` — fwrite 存盘 + 文件 rotate
- `daq_config.h/cpp` — JSON 配置解析
- `main_sim.cpp` — SIM 模式入口
- `main_storage.cpp` — 硬件模式入口
- `extern/circular_buffer/` — SPSC ring buffer

### API 迁移要点

| 旧 API | 新 API |
|--------|--------|
| `onIdle()` | `if (msg.cmd.empty())` in `handleState()` |
| `sleep_for(10ms)` idle | `cv_.wait_for(50ms)` |
| `post()` 无唤醒 | `post()` + `cv_.notify_one()` |
| `idleDelayMs_ = 10` | 移除 |

## 构建命令

```bash
# 本地（无 EPICS）
cd MSM/module_daq
cmake -B build -DMSM_ENABLE_EPICS=OFF
cmake --build build
mkdir -p build/config
cp config/app_inittial_config.json build/config/
./build/msm_daq_sim

# 远程（有 SDK，无 EPICS）
cd MSM_DAQ/module_daq  # 或对应目录
cmake -B build -DMSM_ENABLE_EPICS=OFF -DMSM_DAQ_ENABLE_HARDWARE=ON
cmake --build build
mkdir -p build/config
cp config/app_inittial_config.json build/config/
sudo ./build/msm_daq_storage
```

## 升级远程步骤

1. **备份**旧 `MSM_DAQ/msm/` 目录
2. **复制**本机 `module_core/` 到远程 → 替代旧 `msm/`
3. **复制**本机 `module_daq/` 到远程 → 替代 MSM_DAQ 各散落模块
4. **更新**远程 `CMakeLists.txt` 引用 `module_core/` 替代旧 `msm/`
5. **构建验证**：`cmake -B build -DMSM_ENABLE_EPICS=OFF && cmake --build build`
6. **硬件测试**：`sudo ./build/msm_daq_storage`

## 后续可扩展

- `-DMSM_ENABLE_EPICS=ON` — EPICS CA 集成（需 EPICS base）
- `-DMSM_ENABLE_ZMQ=ON` — ZMQ 网关（默认 ON）
- `-DMSM_ENABLE_HTTP=ON` — REST API + Web GUI（默认 ON）
- DataChannel — TCP 高吞吐传输（从 `module_datachannel/` 合并）
