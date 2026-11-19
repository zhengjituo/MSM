# SHINE_CSM 开发任务列表

## 项目结构

```
SHINE_CSM/
├── AGENTS.md
├── docs/
│   └── design.md
├── tasks/
│   └── YYYYMMDD_*.md       # 各 Agent 工作日志
└── module_core/             # 框架核心模块（当前开发）
    ├── AGENTS.md
    ├── CMakeLists.txt
    ├── include/
    └── src/
```

---

## Task 优先级

| 优先级 | 内容 | 负责模块 |
|--------|------|----------|
| P0 | Message 结构 + NI 解析器 | module_core |
| P0 | MessageQueue 队列实现 | module_core |
| P0 | Module 基类（复用 ThreadStateMachine） | module_core |
| P1 | 外部消息注入 ✅ | module_core |
| P1 | 广播 + 订阅表 | module_core |
| P2 | MessageBus 路由 | module_bus（待建） |
| P3 | 示例设备驱动 | module_example（待建） |
| P3 | MQTT/HTTP Gateway | module_gateway（待建） |

---

## 当前开发：module_core

### Task 1: Message 结构 + NI 字符串解析器

**目标**：实现核心消息结构，支持 NI 字符串解析

**输入**：design.md 第 2、4 节

**交付**：
- `include/Message.h`
- `src/Message.cpp`
- `src/NIStringParser.cpp`

**验收**：
```
输入: "Calculate >> x=10,y=20 -@ SubModule"
输出: Message{cmd:"Calculate", data:{x:10, y:20, _target:"SubModule", _await:true, ...}}
```

---

### Task 2: MessageQueue 实现

**目标**：线程安全消息队列，支持优先级、peek、容错

**输入**：design.md 第 7 节

**交付**：
- `include/MessageQueue.h`
- `src/MessageQueue.cpp`

**验收**：
- EXIT > Response > 普通消息优先级
- peek 不取出
- stop() 后消息自然丢弃

---

### Task 3: Module 基类

**目标**：基于 ThreadStateMachine 扩展

**输入**：design.md 第 10 节

**交付**：
- `include/Module.h`（继承 ThreadStateMachine）
- `src/Module.cpp`
- start() / stop() / handleState()

**验收**：
- 复用 ThreadStateMachine 线程管理
- 支持 postMessage / postResponse

---

### Task 4: 外部消息注入

**目标**：支持外部进程（如 Python）向运行中的 MSM 模块发送消息

**输入**：当前任务描述

**交付**：
- `Module::injectMessage()` 接口
- 外部消息队列 `external_queue_`
- runLoop 中优先处理外部消息

**验收**：
- 外部程序可通过 TCP/ZeroMQ 发送 JSON 消息
- MSM 模块能接收并处理外部消息
- 不影响现有模块运行

---

### Task 5: 同步调用 sendAndWait

**目标**：实现 `-@` 消息类型

**输入**：design.md 第 5 节

**交付**：
- sendAndWait() 阻塞等待
- 超时处理
- emergency_stop_ / wait_interrupt_ 中断

**验收**：
- 阻塞等待 Response
- peek 检测 + 高优先级响应

---

### Task 5: 广播 + 订阅表 ✅

**目标**：实现 `-> <broadcast>` 和订阅机制

**输入**：design.md 第 6 节

**交付**：
- SubscriptionTable 数据结构
- 订阅/取消订阅处理
- 广播分发逻辑

**验收**：
- 广播消息正确分发到订阅者
- 模块退出自动清理

---

## 未来模块（待建）

### module_bus

- MessageBus 路由核心
- 模块注册管理
- 路由分发

### module_example

- 虚拟 SCPI 设备驱动
- 串口/网络设备示例

### module_gateway

- MQTT Gateway
- HTTP Gateway
- TCP/UDP Gateway