# Task 013 — webui 优雅退出：svr.stop() 修复 join 卡死 + SIGHUP 支持

## 起始 commit
```
2b4b395 task 012: close task with end commit
```

## 背景 / 根因
- 用户反馈 msm_webui 按 Ctrl+C 退不出来
- 根因：`httpThread` 线程跑 `svr.listen("0.0.0.0", 8080)`，永不返回；主流程在
  `httpThread.join()` 前从未调用 `svr.stop()`，`g_stop=true` 后进程卡死在 join
- 另外：webui 只注册了 SIGINT/SIGTERM，没有 SIGHUP，关终端窗口（SIGHUP）会直接
  被默认动作杀掉，不走优雅关闭

## TODO
- [x] `main_webui/main_hwdaq.cpp`: 主流程 join 前调用 `svr.stop()` 解开 listen 阻塞
- [x] 注册 `signal(SIGHUP, ...)` 与 SIGINT/SIGTERM 一致，触发 `g_stop` 优雅关闭
- [x] 编译验证 main_webui 通过
- [x] git commit + 完结 task

## 日志

### 设计决议
- `svr.stop()` 在 main 线程调用（与 listen 不同线程），标准 cpp-httplib 用法；
  idle interval 默认 10ms，stop 后 listen 循环 ~10ms 内退出，join 立即返回
- SIGHUP 处理与 SIGINT/SIGTERM 相同：置 `g_stop`，走
  `ipc.close() → stopBackend() → svr.stop() → httpThread.join()` 完整优雅链
- 推荐退出方式：终端里 Ctrl+C（SIGINT）即可优雅退出到 shell；关终端窗口
  （SIGHUP）同样优雅关闭

## 结束 commit
```
bab9cf6 fix: webui graceful exit - call svr.stop() before httpThread.join() (unblocks listen), add SIGHUP handler
```
