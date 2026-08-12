# Task 012 — webui 后台进程优雅退出，确保 xdma 资源释放

## 起始 commit
```
c32bdb0 docs: rewrite README as GitHub entry (CSM-inspired C++ framework + DAQ example)
```

## 背景 / 根因
- web「Quit」→ `POST /api/stop` → `ipc.send("QUIT")` + `stopBackend()`
- `stopBackend()` 对后台进程发 SIGTERM 后只 `waitpid` 2 秒，超时直接 SIGKILL
- 后台进程**唯一**释放 xdma 的路径是优雅关闭：`daq.stop()` → `closeHW()` → `CxDeviceSdkDeinit()`
- 真实硬件关闭耗时可能 >2s，SIGKILL 会把正在清理 xdma 的进程强杀，资源不释放
- 关闭 webui 终端时再次调用 `stopBackend()`，重复同样风险

## TODO
- [x] `main_webui/main_hwdaq.cpp`: `stopBackend()` 改为先发 SIGTERM，等子进程自然退出（waitpid 轮询，预算 ~10s），超时才 SIGKILL 兜底，并在兜底前打印提示
- [x] `/api/stop` 保持先发 QUIT 再 stopBackend（QUIT 与 SIGTERM 都走同一 g_stop 优雅路径）
- [x] 编译验证 main_webui 通过
- [x] git commit + 完结 task

## 日志

### 设计决议
- SIGKILL 只在优雅预算超时（进程疑似挂死）时使用，作为最后兜底
- 优雅预算 10s，覆盖 stopChannels → flushAll → CxDeviceClose ×N → CxDeviceSdkDeinit 的完整关闭链
- `stopBackend()` 同步回收子进程后才返回，`/api/stop` 响应晚到但保证状态一致
- 不触碰 `config/*.json`（用户当前硬件 slot 配置，单独保留）

## 结束 commit
```
207690b fix: stopBackend waits up to 10s for graceful closeHW before SIGKILL (release xdma)
```

## Session Git Log

### 2026-08-12  webui 后台进程优雅退出
```
207690b fix: stopBackend waits up to 10s for graceful closeHW before SIGKILL (release xdma)
2b4b395 task 012: close task with end commit
```

