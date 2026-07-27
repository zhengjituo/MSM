# Task 008 — ConfigModule：可复用的 MSM 配置管理模块

## 起始 commit
```
b48e799 task: detail 008 AppConfig implementation steps for handoff
```

## 设计目标

ConfigModule 是一个纯 MSM 模块，专司配置的加载/保存/分发/参数访问，不依赖任何硬件或采集模块。
其他项目只需替换内部的 `AppConfig` 解析逻辑（或通过配置文件定义字段映射），即可复用整套消息接口。

### 消息接口

| 消息 | 效果 |
|------|------|
| `LoadFromJson >> path=/config/app_runtime_config.json` | 读 JSON → 更新内存 |
| `SaveToJson >> path=/config/app_runtime_config.json` | 内存 → 写 JSON |
| `Distribute >> -@ target` | 向目标模块发 Configure 消息 |
| `SetParam >> /ch1/trigLength=512` | 改单参数（运行时） |
| `GetParam >> /ch1/trigLength` | 返回参数值 |

### 模块结构

```
module_config/
  CMakeLists.txt
  mod_config.h/.cpp          ← ConfigModule（MSM 模块派生）
  config_impl.h/.cpp         ← AppConfig 数据结构（loadFromJson/saveToJson）
  config_impl.h              ← 可替换，其他项目只需重新实现这个接口
```

## TODO
- [ ] 创建 `module_config/` 目录

      mkdir /home/zkcx/ws/MSM_DAQ/module_config

- [ ] 创建 `CMakeLists.txt`

      add_library(module_config mod_config.cpp config_impl.cpp)
      target_include_directories(module_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
      target_link_libraries(module_config msm_core)
      # 根 CMakeLists.txt 加 add_subdirectory(module_config)

- [ ] 搬移并重构配置数据结构

      cp module_daq/daq_config.h module_config/config_impl.h
      cp module_daq/daq_config.cpp module_config/config_impl.cpp
      # class DaqConfig → class ConfigImpl（命名暗示可替换）
      # 保留 loadFromJson()、toKeyValues()、GetFrameLength() 等
      # 新增 saveToJson()

- [ ] 实现 ConfigModule（MSM 模块）

    - `LoadFromJson` → `configImpl_.loadFromJson(path)`
    - `SaveToJson` → `configImpl_.saveToJson(path)`
    - `Distribute` → 遍历配置，向 DAQModule/Process/Storage 发 Configure 消息
      - DAQModule: 发 Init 或 Configure（带 toKeyValues() 拍平的键值对）
      - Process: BatchSize + FrameSize + StorageTarget
      - Storage: SavePath + FelName + SystemName + DeviceName + FileType + MaxSaveSizeMB + ExpNo + RunSerial + BatchSize + FrameSize + Slot
    - `SetParam /path=value` → 解析路径，更新 configImpl_ 对应字段
    - `GetParam /path` → 返回对应字段值

- [ ] main_hwdaq / main_simsrc 重构

    - main 创建 `ConfigModule`，register、start
    - 不再声明全局 `g_config`，不再 `loadFromJson` / `distributeConfig` 硬编码
    - 首次启动: 发 `LoadFromJson >> path=...` + `Distribute >> -@ *`（或逐个发）
    - start 流程: `Distribute >> -@ DAQModule`（带 isInit 标记）→ ...

- [ ] 编译验证

      cd /home/zkcx/ws/MSM_DAQ/main_cli/build && cmake ../.. && make -j$(nproc)
      # msm_cli_simsrc + msm_cli_hwdaq 均通过

- [ ] 独立调试验证

      # 启动只有 ConfigModule + IPCServer 的最小 main
      # 通过 msm_ipc_shell 发 LoadFromJson / SetParam / GetParam / SaveToJson
      # 确认 JSON 正确读写，参数正确修改

- [ ] 运行测试

      (echo "start"; sleep 3; echo "stop"; sleep 1; echo "quit") | ./msm_cli_simsrc
      # trig_id 跨通道一致，无 crash

## 日志

### 设计决议
- ConfigModule 是纯 MSM 模块，不依赖硬件
- 内部 `ConfigImpl` 负责 JSON 解析，可替换
- main 对配置无直接 C++ 访问（通过消息），除非必要才暴露 getter
- 其他项目：复制 `module_config/`，替换 `config_impl.h/.cpp` 即可复用消息协议

### 已完成
- WaveDataEnable=0 全面支持
- saveRing 256MB，main 统一管理 rings
- hwdaq Configure 顺序修复
- Storage/Process idle 10ms + saveRing 原子写入
- drain→flush 重命名 + discard 日志
- config 目录迁移到顶层
- module_tui 删除

## Done

## 结束 commit
```
```