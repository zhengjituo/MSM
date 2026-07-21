# Task 006 — V1: Storage discard mode（读取丢弃，不写盘）

## 起始 commit
```
6b079e6 docs: add superpowers to archive index
```

## TODO
- [x] 分析现状
- [ ] V1 main: 去掉 `storage.start()` + `Start >> Storage` 消息
- [ ] 编译验证
- [ ] git commit

## 日志
- process_full 已完成对齐检查，peek 全帧并打印第一帧 TrigID/BunchID ✅
- storage 默认 `recording_=false`，`onSaveBatchAll` 已按 `recording_` 分支：false 则 discard ✅
- 只需不让 storage 进入录制模式即可

## Done

## 结束 commit
```
2d14a06 gitignore: exclude build5 artifacts, re-untrack
```
