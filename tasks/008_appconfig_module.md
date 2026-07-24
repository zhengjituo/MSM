# Task 008 — AppConfig 配置管理模块

## 起始 commit
```
f560c4b (tag: milestone-20260724_wavedataenable_0)
config: move app_initial_config.json to top-level config/ for shared access
```

## TODO
- [ ] 创建 `module_config/` 目录及 CMakeLists.txt

      cd /home/zkcx/ws/MSM_DAQ && mkdir module_config
      # CMakeLists.txt: add_library(module_config daq_config.cpp)
      # target_include_directories(module_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
      # 根 CMakeLists.txt 加 add_subdirectory(module_config)

- [ ] 将 `module_daq/daq_config.h/cpp` 移入 `module_config/`，类名 `DaqConfig` → `AppConfig`

      mv module_daq/daq_config.h module_config/app_config.h
      mv module_daq/daq_config.cpp module_config/app_config.cpp
      # class DaqConfig → class AppConfig
      # 保留所有字段和 loadFromJson()，新增 saveToJson()、distribute()

- [ ] AppConfig 实现：

  - `saveToJson(path)` — 将当前配置写为 JSON，与 loadFromJson 格式对称
  - `distribute()` — 遍历内部配置，向 DAQModule/Process/Storage 发 Configure 消息
    - DAQModule: 调用 toKeyValues() 发 Init 或 Configure（isInit 参数）
    - Process: BatchSize + FrameSize + StorageTarget
    - Storage: SavePath + FelName + SystemName + DeviceName + FileType + MaxSaveSizeMB + ExpNo + RunSerial + BatchSize + FrameSize + Slot
  - 保留 `toKeyValues()` 方法（当前 daq_config.h 已有）

- [ ] main_hwdaq / main_simsrc 重构：

  - 全局 `g_config` → `AppConfig` 实例
  - `start` 流程：`appConfig.loadFromJson(runtimePath)` → `appConfig.distribute(isInit=false)` → 发 Start
  - 提取 main_simsrc.cpp 和 main_hwdaq.cpp 中共用的分发逻辑到 AppConfig::distribute()
  - main 只负责：创建模块、register、start、编排命令流程

- [ ] 编译验证

      cd /home/zkcx/ws/MSM_DAQ/main_cli/build && cmake ../.. && make -j$(nproc)
      # msm_cli_simsrc + msm_cli_hwdaq 均通过

- [ ] 短期运行测试

      (echo "start"; sleep 3; echo "stop"; sleep 1; echo "quit") | ./msm_cli_simsrc
      # trig_id 跨通道一致，无 crash

- [ ] EPICS 桥接地基（可选，可后续单独 task）

  - `updateConfigToPV()` 接口声明
  - PV 到 AppConfig 字段的路径映射表

## 日志

### 设计决议
- AppConfig 内存为 master，JSON 为持久化快照，EPICS PV 为视图
- 无额外 CLI 命令：`start` = load JSON → AppConfig → 配硬件 → 运行
- EPICS caput → 直接更新 AppConfig → 自动 `saveToJson()`
- `module_daq/daq_config.h` 的解析逻辑完全保留，仅移动位置
- Json 兼容官方软件 `app_initial_config.json` 格式

### 上下文交接（2026-07-24，token 将满）

已完成的历史：
1. WaveDataEnable=0 全面支持（frameSize 自动推导，所有模块统一）
2. saveRing 256MB，main 统一管理 rings
3. hwdaq Configure 顺序修复（先注册后分发）
4. Storage while 循环消费 + idle 10ms + discard 日志
5. Process idle 50ms→10ms + saveRing 原子写入
6. 全代码 drain→flush 重命名
7. config 目录迁移到顶层 `config/`
8. `module_daq/config/` 清理（已空）

关键文件：
- `config/app_initial_config.json` — 配置模板
- `module_daq/daq_config.h/cpp` — 待移入 `module_config/`
- `module_daq/module_daq.h/cpp` — Init/Configure/Start/Stop 流程，**不要动**
- `main_cli/main_hwdaq.cpp` — hardware 主流程
- `main_cli/main_simsrc.cpp` — simulation 主流程
- `module_storage/module_storage.cpp` — flush/discard 已改好
- `module_process/module_process.cpp` — idle 10ms, 原子写入 saveRing

## Done

## 结束 commit
```
```