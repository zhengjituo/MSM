# 004 — enablesave/disablesave + saveRing 对齐修复

## TODO

- [x] ModuleStorage 加 `saveEnabled_` 布尔量 + `onEnableSave()`/`onDisableSave()`（纯设 bool，无文件操作）
- [x] `onStart()` 根据 `saveEnabled_ && nCh_>0` 决定是否开文件
- [x] 恢复 V1 start/stop 时序（Storage → Process → DAQModule / DAQModule → Process → Storage）
- [x] IPCServer 裸命令 + IPC shell 支持 enablesave/disablesave
- [x] Storage 每批打印首帧 trig_id/bunch_id 调试
- [x] 同步 rotateFile（所有 channel 一起转，避免 sequence 错位）
- [x] Process 写 saveRing 前检查剩余空间，不够则跳过整轮（防止 buffer 满时部分写出导致 cycle 错位）
- [x] 硬件实测验证

## 修改文件

| 文件 | 改动 |
|------|------|
| `module_storage/module_storage.h` | 加 `saveEnabled_` 字段、`onEnableSave/onDisableSave` 声明 |
| `module_storage/module_storage.cpp` | 实现新方法、同步 rotate、每批 trig_id/bunch_id 调试 |
| `module_process/module_process.cpp` | 写 saveRing 前检查 `availableSpace()` |
| `module_core/src/IPCServer.cpp` | START/STOP 含 Storage，新增 ENABLESAVE/DISABLESAVE 裸命令 |
| `module_main_v1/msm_ipc_shell.cpp` | 新增 enablesave/disablesave 裸命令 |
| `module_main_v1/main_hwdaq.cpp` | CLI + shutdown 适配 |
| `module_main_v1/main_simsrc.cpp` | CLI + shutdown 适配 |
| `module_daq/daq_config.h/cpp` | V2 字段（slot 派生等） |

## 硬件测试结果

- trig_id 逐帧 +1 ✓
- bunch_id 逐帧 +10 ✓
- 文件名 BunchID 全部通道对齐 ✓
- 无 .tmp 残留 ✓

## 结束 commit

`TBD`
