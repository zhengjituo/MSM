# Task Log

## 2026-06-18 MSG 驱动改造 + 配置扁平化

### 问题
- `data.data = toKeyValues()` 覆盖了 `_target`/`_sender` 内部键，MSG 被静默丢弃
- `msm_daq` (`main.cpp`) 无独立用途，已删除
- Storage 模块不录文件时不消费 ring buffer，有溢出风险

### 修复
1. `main_storage.cpp` / `main.cpp`: `setTarget`/`setSender` 移到 `data.data` 赋值之后
2. 删除 `Module_main/main.cpp` + CMake 中 `msm_daq` target
3. `module_storage.cpp`: `onSaveBatchAll` 总消费 ring，仅在 `recording_` 时写文件
4. AGENTS.md 清理过期引用
5. 各模块头文件补充类注释

### 提交
```
ff526c1 Storage: always consume ring, only write file when recording
513a58b 补充类注释及过期注释修复
876a299 dataSavePath: /mnt/localssd/daq_data (subdir, not root)
55c6f37 backup: message-driven refactor complete
```

### 测试
- 模拟模式: 106 万帧/通道, overflow=0 (正常录文件)
- 模拟模式: 停止 Storage 后 overflow=0, saved=0 (消费不存盘)
- 硬件模式: 19 万帧/通道, 3010 触发正常, flush 捕获残余数据

### 已知问题
- `Module_main/` 目录名未统一为 `module_main/`
- `app_inittial_config.json` 拼写错误
- `doc/architecture.md` 描述已过时
- `msm::JsonValue` / `msm::MessageQueue` 未使用死代码
- `lastWritePos_` 使用 int64_t 跟踪 uint64_t 计数器
