# module_process — 处理模块（轻量 peek 版）

## 功能简介
从 ring buffer 批量 peek 帧，提取首帧 TrigID/BunchID 用于实时监控，转发 SaveBatchAll 给 Storage。

## 输入
- `ConsumeOnce` — 消费一个 batch
- `Configure` — BatchSize、StorageTarget
- `Start` / `Stop` — 启停
- `Status` — 状态查询

## 输出
- `SaveBatchAll` → Storage 模块
- 首帧 TrigID/BunchID 日志
- 可通过 getter 查询通道帧数、batch 数、最后 TrigID/BunchID

## 状态帧说明

| 帧 | 功能 |
|----|------|
| Start | 清零计数器，开始消费 |
| Stop | 停止，打印各通道帧数 |
| ConsumeOnce | peek 首帧 → TrigID/BunchID → Send SaveBatchAll |
