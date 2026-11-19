# Task 002_1 — 扩展 MessageBus 支持外部消息注入

> 2026-04-22（日期制时代早期任务，原文件名 `20260422_01_external_injection.md`）

## 状态
- [x] 已完成

---

## todo: 计划
### 需求
当前 MSM 的 MessageBus 是进程内内存队列通信，外部测试程序（如 Python）无法直接发送消息给已运行的 MSM 模块。

需要扩展 MessageBus，使其支持：
1. 外部进程通过 ZeroMQ 发送消息
2. 内部消息调度器能接收外部消息
3. 非侵入式扩展，不影响现有模块

### 目标
1. 在 MessageBus 中添加外部消息源注册接口
2. 实现 ZeroMQ 桥接模块（外部可选）
3. 外部测试脚本可通过 TCP 发送 JSON 消息
4. 不修改现有 MSM 核心代码

### 设计
```
[MSM Service]
  ├─ MessageBus (进程内)
  ├─ AddModule
  └─ ZmqBridge (新增)
      └─ ZeroMQ TCP Server (5555)
          └─ 外部 Python 客户端连接

[External Python]
  └─ ZMQ REQ Socket → tcp://localhost:5555
      └─ 发送 {"cmd": "Action", "target": "AddModule"}
```

#### MessageBus 扩展接口
```cpp
class MessageBus {
public:
    // 注册外部消息源
    void registerExternalSource(const std::string& name,
                               std::function<bool(Message&)> source);
    
    // 等待消息（支持外部源）
    bool waitMessage(Message& msg, int timeout_ms);
};
```

### 修改范围
- `module_core/include/MessageBus.h`
- `module_core/src/MessageBus.cpp`
- 新增 `module_core/src/ZmqBridge.cpp` (可选)

---

## done: 结果

（待完成）