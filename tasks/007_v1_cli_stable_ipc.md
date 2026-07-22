# Task 007 — V1 CLI stable + IPC 交互 / stop drain + ready 信号

## 起始 commit
```
5af5ec3 fix: untrack runtime_config.json
```

## 变更总览

### module_daq
- `onStart` 移除 2s settle + pre-drain（不再等待不丢帧）
- `onStop` 重排序：先 `flushAll()` 残帧写入 ring，再发 Stop 给 Process drain
- `flushAll()` 替代 discard drain，残帧计入 `totalFrames`
- `Init` 跳过二次 `initHW`

### module_process_full
- drain 模式：逐通道 `availableFrames()` 读光，不满 batchSize 也传
- 所有通道空时 drain 完成，打印 `ready for start`

### module_main_v1
- 取消自动 start：启动后等待用户命令 `start` / `stop`
- quit 不卡住：`close(STDIN_FILENO)` 释放 CLI getline
- 新增 `msm_ipc_shell`：IPC 独立终端，`quit` 发服务器、`status` 大写+target

### IPCServer
- MSG handler 支持 `isAwaitResponse()`：用 `sendAndWait` 返回模块响应

## TODO
- [x] onStart: 移除 2s settle + pre-drain
- [x] onStop: 重排序 flushAll → Stop Process
- [x] flushAll() 替代 discard
- [x] Process drain 模式：逐通道读光
- [x] main_hwdaq: 去自动 start，用户 start/stop 命令
- [x] main_hwdaq: quit close stdin 不卡住
- [x] IPCServer: MSG sendAndWait 响应
- [x] msm_ipc_shell: quit 发服务器、status 大写+target
- [x] "ready for start" 消息
- [x] 编译验证
- [x] git commit + tag 里程碑

## 日志
- 实测 start→stop→start 四通道 daq_frames / proc_frames 完全一致
- Drain complete 后明示 "ready for start"

## 结束 commit
```

```