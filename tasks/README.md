# Task 历史

## 2025-04

### 20250407_01: Message + NI Parser + Queue ✅

- 实现 Message 结构（cmd + data）
- 实现 NI 字符串解析器（parseNI）
- 实现 MessageQueue（优先级、peek）

---

### 20250407_02: Module 基类 ✅

- 实现 Module 抽象基类
- handleState() 接口
- start()/stop() 生命周期管理
- 内部线程 + 消息队列

---

### 20250408_03: MessageBus 路由核心 ✅

- registerModule() / unregisterModule()
- postMessage() 路由
- 同步调用 handleState + Response

---

### 20250408_03_01: 同步调用 Timeout 修复 ✅

- post() 不等待，sendAndWait() 轮询
- 超时返回 Timeout
- EmergencyStop 中断等待

---

### 20250408_03_02: DryRun 测试模式 ✅

- setDryRun(true) 自动响应
- 无需真实硬件即可测试流程

---

### 20250408_03_03: 响应处理 ✅

- Response 消息通过 handleState 处理
- 不占用主消息循环

---

### 20250408_04: 简化 post() 接口 ✅

- 支持 NI 字符串直接调用
- `post("API >> args -@ Target")`

---

### 20250408_04: 广播订阅 ✅

- `<register>` / `<unregister>` 订阅表
- `<broadcast>` 分发
- bySubscriber 清理

---

### 20250408_05: 构造参数 + 自动注销 ✅

- Module 构造函数传 name
- stop() 自动从 MessageBus 注销
- bySubscriber 清理订阅

---
