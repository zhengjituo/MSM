# MSM 设计文档

## 1. 概述

基于 NI CSM (Communicable State Machine) 的消息驱动状态机框架，C++ 实现。

### 核心目标

- 模块间消息驱动解耦（不 import/头文件依赖）
- 支持同步/异步调用、广播、订阅
- 业务逻辑下沉边缘，不依赖中央采集
- 支持 NI CSM 字符串语法作为一等公民

---

## 2. 消息结构

### Message 结构

```cpp
struct Message {
    string cmd;    // 唯一必需字段，命令名
    json data;     // 全部数据（业务参数 + 路由元数据）
};
```

### 路由元数据约定

框架内部使用 `_` 前缀字段，业务代码不感知：

```json
{
    "cmd": "Calculate",
    "data": {
        "x": 10,
        "y": 20,
        "_sender": "MainModule",
        "_target": "SubModule",
        "_correlationId": 12345,
        "_await": true,
        "_timeout": 5000,
        "_mode": "call",
        "_remote": "Beta"
    }
}
```

| 字段 | 说明 |
|------|------|
| _sender | 发送方模块名 |
| _target | 目标模块名或特殊标记 |
| _correlationId | 关联ID，用于响应路由 |
| _await | 是否等待响应 |
| _timeout | 超时毫秒数，-1 无限等待 |
| _mode | call/broadcast/register/unregister |
| _remote | 目标对等节点名（跨进程时设置） |

---

## 3. 消息类型

### 3.1 消息类型自动判断

框架根据 NI 字符串符号或内部字段自动判断类型：

| 符号 | 类型 | await | 说明 |
|------|------|-------|------|
| `-@` | 同步调用 | true | 阻塞等待 Response |
| `->` | 异步有返回 | true | 不阻塞，等待 Async Response |
| `->|` | 异步无返回 | false | 不阻塞，无响应 |
| `-> <broadcast>` | 广播 | - | 1 对多分发 |
| `-><register>` | 订阅 | - | 注册状态订阅 |
| `-><unregister>` | 取消订阅 | - | 取消订阅 |

### 目标特殊标记

| target | 说明 |
|--------|------|
| `<broadcast>` | 广播 |
| `<register>` | 订阅 |
| `<unregister>` | 取消订阅 |

---

## 4. NI 字符串解析

### 语法

```
API >> args -@ Target        // 同步调用
API >> args -> Target        // 异步有返回
API >> args ->| Target      // 异步无返回
API >> args -> <broadcast>   // 广播
API >> args -><register>    // 订阅
```

### 解析示例

```
输入: Calculate >> x=10,y=20 -@ SubModule

输出 Message:
{
    "cmd": "Calculate",
    "data": {
        "x": 10,
        "y": 20,
        "_sender": "当前模块名",
        "_target": "SubModule",
        "_correlationId": 12345,
        "_await": true,
        "_timeout": -1,
        "_mode": "call"
    }
}
```

---

## 5. 同步调用实现

### 发送

```cpp
Message msg = parseNI("GetConfig >> key=threshold -@ SubModule");
sendAndWait(msg, 5000);  // 框架内部处理
```

### 实现

```cpp
Message sendAndWait(Message& msg, int timeout_ms) {
    msg.setCorrelationId(generateCorrelationId());
    msg.setAwaitResponse(true);
    
    std::promise<Message> promise;
    pendingResponses_[cid] = promise;
    
    send(msg);
    
    if (future.wait_for(timeout_ms) == ready) {
        return future.get();
    }
    return Timeout;
}
```

---

## 6. 广播与订阅

### 订阅表结构

```cpp
struct SubscriptionTable {
    // api -> source -> list of subscribers
    unordered_map<string, unordered_map<string, vector<string>>> subs;
    
    // subscriber -> list of (api, source) 用于退出时清理
    unordered_map<string, vector<pair<string, string>>> bySubscriber;
};
```

### 广播分发

本地广播发送给所有订阅者 + 自动转发给 MSMGateway 跨进程发布：

```cpp
void broadcast(Message& msg) {
    // 本地订阅者分发...
    
    // 转发给 MSMGateway 进行跨进程 PUB
    Module* gw = findModule("MSMGateway");
    if (gw) gw->post(msg);
}
```

### 模块退出清理

```cpp
void onModuleExit(string moduleName) {
    // 从订阅表移除该模块的所有订阅
}
```

---

## 7. 消息队列

### 实现

```cpp
class Module {
    void runLoop() {
        while (running_) {
            Message msg;
            { lock(); msg = queue_.pop(); }
            if (!msg.cmd.empty())
                handleState(msg);
            else {
                handleState(Message{});  // 空闲帧，模块在此轮询
                sleep(50ms);
            }
        }
    }
};
```

模块每隔 50ms 会在无消息时收到空消息帧，可用于轮询内存变量（如 action/callRemote 触发器）。

---

## 8. 跨进程通信 (MSMGateway)

### 架构

```
┌─ 本地 MessageBus ──────────────────────────────────┐
│                                                     │
│  ModuleA ──→ MessageBus ──→ ModuleB                 │
│                │                                    │
│                │ (如果 _remote 标记)                  │
│                ▼                                    │
│           MSMGateway                                │
│  ┌────┬────┬────┬────┐                             │
│  │REP │DEALER│PUB│SUB │                             │
│  └──┬─┴──┬──┴──┬┴──┬─┘                             │
│     │    │     │   │                                 │
└─────┼────┼─────┼───┼─────────────────────────────────┘
      │    │     │   │
  入(收)│出(发)│广播│订阅│
      │    │     │   │
      ▼    ▼     ▼   ▼
    TCP   TCP   TCP  TCP
```

### 四套接字

| 角色 | ZMQ 类型 | 配置字段 | 方向 | 说明 |
|------|----------|----------|------|------|
| 入 (Ingress) | REP | `rep_port` | bind | 收外部请求/对端 DEALER |
| 出 (Egress) | DEALER | `peers[].endpoint` | connect | 发往对端 REP |
| 广播 (Publish) | PUB | `pub_port` | bind | 发布变量变更 |
| 订阅 (Subscribe) | SUB | `subscriptions[].endpoint` | connect | 接收远程广播 |

### 路由流程

```
ModuleA 发送 _remote="Beta" 消息:
  
  MessageBus::send()
    → msg.isRemote() == true
    → findModule("MSMGateway") → gw->post(msg)
    → MSMGateway::handleState()
      → 查找 peer "Beta" 的 DEALER socket
      → zmq_send(DEALER, json)
      → TCP → 对端 REP socket
        → MSMGateway::runREP()
          → MessageBus::sendAndWait() → 本地 ModuleB
          → 响应经 REP 原路返回
        → DEALER 收到 response
          → notifyResponse() → 原 sendAndWait 解除阻塞
```

### 广播/订阅流程

```
ModuleA 广播 PV_UPDATE:
  
  MessageBus::broadcast()
    → 本地订阅者分发
    → gw->post(msg)
    → MSMGateway::handleState()
      → push 到 pubQueue
      → runPUB(): zmq_send(PUB, json)
      → TCP → 对端 SUB socket
        → MSMGateway::runSUB()
          → 匹配 subscriptions.bindings
          → mod->getVars()->set(localVar, value)
```

### JSON 配置 (每台设备一份)

```json
{
    "gateway": {
        "rep_port": 5555,
        "pub_port": 5557,
        "rest_port": 8080,
        "peers": [
            {"name": "Beta", "endpoint": "tcp://192.168.1.102:5556"}
        ],
        "subscriptions": [
            {
                "name": "Beta",
                "endpoint": "tcp://192.168.1.102:5558",
                "bindings": [
                    {"remote_var": "MultiplyModule:result", "local_var": "AddModule:remoteResult"}
                ]
            }
        ]
    },
    "modules": [...]
}
```

| 字段 | 说明 |
|------|------|
| peers[].name | 对端节点名，_remote 引用 |
| peers[].endpoint | 对端 REP 地址 (tcp://ip:port) |
| subscriptions[].endpoint | 对端 PUB 地址 |
| subscriptions[].bindings | 远程变量 → 本地变量映射 |

---

## 9. 接口服务层

所有接口服务遵循同一模式：继承 `Module`，加载 `msm_vars.json` 配置。

### 架构

```
msm_vars.json ──→ 内存偏移描述
                      │
           ┌──────┬──────┬──────┬──────────┐
           ▼      ▼      ▼      ▼          ▼
       IPCServer PVServer MSMGateway  RESTServer
       UNIX Skt  PVA     ZMQ REP+PUB   HTTP/WS
                         DEALER+SUB
```

### 9.1 IPCServer

- **协议**: UNIX domain socket
- **访问方式**: 变量级 (GET/PUT/LIST/SCHEMA)
- **数据路径**: 内存偏移直接读写
- **自动发现**: start() 时扫描 MessageBus 已注册模块并自动映射

### 9.2 PVServer

- **协议**: EPICS PVA (in-process ServerContext) / softIOC
- **访问方式**: 变量级 (caget/caput)
- **数据路径**: 内存偏移直接读写
- **消息订阅**: 通过 PV_UPDATE 消息同步

### 9.3 MSMGateway

- **协议**: ZMQ (REP + DEALER + PUB + SUB)
- **访问方式**: 消息级 (JSON)
- **数据路径**: MessageBus 路由 + 内存偏移 (SUB 端)
- **跨机器**: ✅ TCP 直连
- **配置**: 从 JSON 加载 gateway 段 (端口、对端、订阅)

详情见第 8 节。

### 9.4 RESTServer

- **协议**: HTTP (cpp-httplib)
- **访问方式**: 变量级 + POST action
- **数据路径**: 内存偏移直接读写
- **自动发现**: start() 时扫描 MessageBus 已注册模块并自动映射

| 方法 | 路径 | 功能 | 数据路径 |
|------|------|------|----------|
| GET | `/api/:module/:var` | 读变量 | 内存偏移 |
| PUT | `/api/:module/:var` | 写变量 | 内存偏移 |
| POST | `/api/:module/action` | 原子操作 | MessageBus |
| GET | `/api/schema` | 列出实际存在的模块变量 | varMap_ |
| GET | `/api/:module/schema` | 模块变量结构 | 配置元数据 |
| GET | `/api/:module` | 模块所有变量当前值 | 内存偏移 |

schema 只返回实际映射到内存的模块（不是 JSON 中定义的全部）。

### 9.5 接口服务对比

| 维度 | IPC | PVA | MSMGateway | REST |
|------|-----|-----|-----------|------|
| 协议 | UNIX Socket | EPICS PVA | ZMQ TCP | HTTP |
| 粒度 | 变量 | 变量 | 消息 | 变量 + 消息 |
| 跨机器 | ❌ | ✅ | ✅ | ✅ |
| 原子操作 | ❌ | ❌ | ✅ | ✅ (POST) |
| 双向 | ❌ | ❌ | ✅ (DEALER/REP) | ❌ (请求-响应) |
| 广播 | ❌ | ❌ | ✅ (PUB/SUB) | ❌ |
| 依赖 | 无 | EPICS | libzmq | cpp-httplib |
| 适用场景 | 本地调试 | EPICS 生态 | MSM 原生互连 | Web/第三方 |

---

## 10. 框架核心组件

### MessageBus (中央邮局)

```cpp
class MessageBus {
    void registerModule(Module* m);
    void unregisterModule(const string& name);
    Module* findModule(const string& name);
    void send(const Message& msg);
    Message sendAndWait(Message& msg, int timeout_ms);
    void subscribe(string api, string source, string subscriber);
    void broadcast(const Message& msg);
    
private:
    map<string, Module*> modules_;
    map<uint64_t, promise<Message>> pendingResponses_;
};
```

`send()` 路由规则：

1. target == `<broadcast>` → broadcast()
2. target == `<register>` → subscribe()
3. target == `<unregister>` → unsubscribe()
4. msg.isResponse() → notifyResponse()
5. msg.isRemote() → MSMGateway()
6. 否则 → findModule(target) → post()

### Module 基类

```cpp
class Module {
    virtual void handleState(const Message& msg) = 0;
    virtual ModuleVars* getVars() = 0;
    virtual void* getVarBase() { return nullptr; }
    virtual size_t getVarSize() { return 0; }
    
    void post(const Message& msg);
    Message post(const string& niString);
    void start();  // 创建线程 + runLoop
    void stop();
    
protected:
    string name_;
    queue<Message> queue_;
    thread* thread_;
};
```

### ModuleVars 接口

```cpp
struct ModuleVars {
    virtual void* get(const string& name) = 0;
    virtual bool set(const string& name, const string& value) = 0;
    virtual map<string, string> list() = 0;
    virtual map<string, string> getTypes() { return {}; }
};
```

接口服务 (RESTServer/IPCServer) 通过 `getVars()->set()` 写入远程广播收到的值。

---

## 11. 设计边界

### 适合的场景

- 业务流程控制（同步等待安全）
- 状态变化通知
- 低频次信息交互
- 设备模块抽象

### 不适合的场景

- 高频原始数据采集（应在设备模块内闭环）
- 毫秒级实时控制（PLC/cRIO/RTOS）

### 扩展路径

| 阶段 | 实现 |
|------|------|
| ✅ 单进程多线程 | 内存队列 |
| ✅ 多进程同主机 | MSMGateway (ZMQ TCP) |
| ✅ 多进程跨主机 | MSMGateway (ZMQ TCP) |
| 待定 | MQTT Gateway 模块 |

---

## 12. DataChannel 高吞吐数据通道

### 12.1 架构

```
Alpha (Producer)                          Beta (Consumer)
┌─────────────────────┐                 ┌─────────────────────┐
│ DCProducerModule    │                 │ DCConsumerModule    │
│  TCP: send(data)    │ ──TCP data──→   │  TCP: recv(data)    │
│  ZMQ: DATA_READY    │ ──ZMQ ctrl──→   │  handleState()      │
│                     │   via MSMGatewy │                     │
│ MSMGateway(REP+PUB) │ ←─────────────→ │ MSMGateway(REP+PUB) │
└─────────────────────┘                 └─────────────────────┘
```

| 路径 | 协议 | 用途 | 特点 |
|------|------|------|------|
| 数据面 | 裸 TCP | 批量数据传输 | 零拷贝预分配 buffer，无解析开销 |
| 控制面 | ZMQ (DEALER/REP) | DATA_READY / DONE 信令 | 非阻塞，不参与数据搬运 |
| 广播 | ZMQ (PUB/SUB) | 变量变更通知 / 心跳 | 可跨网络 |

### 12.2 数据流

每 batch 的时序：
1. Producer: 发 `DATA_READY`（ZMQ DEALER → 对端 REP）
2. Producer: `send()` 批量数据到 TCP 连接
3. Consumer: 收到 `DATA_READY` → `recvAll()` 从 TCP 缓冲区读取

TCP 连接在 `start()` 时建立（Producer accept / Consumer connect），
整个生命周期复用，不每 batch 重连。

### 12.3 网络韧性

| 机制 | 实现 | 说明 |
|------|------|------|
| 发送前健康检查 | `getsockopt(TCP_INFO)` | 0.5μs，检测连接终结态 |
| 超时 | `SO_SNDTIMEO/SO_RCVTIMEO` 5s | 防永久阻塞 |
| Keepalive | `SO_KEEPALIVE` | 检测死连接 |
| 重联 | `reconnect()` | 失败后 close + retry start() |

失败时 Producer 丢弃当前 batch 数据，标记错误，尝试重联。
**不阻塞数据采集源头。**

### 12.4 性能

| 场景 | batch 大小 | 总数据量 | 吞吐 | 瓶颈 |
|------|-----------|---------|------|------|
| localhost | 11.52MB | 11.5GB | **3.5 GB/s** | CPU/内存带宽 |
| 跨网 (1 Gbps) | 11.52MB | 1.15GB | **111 MB/s** | 网卡线速 |

### 12.5 配置

```json
{
  "module": "DCProducer",
  "dc_producer": {
    "data_port": 9100,
    "frames_per_batch": 10000,
    "frame_size": 1152,
    "total_batches": 100
  },
  "gateway": {
    "rep_port": 5555,
    "pub_port": 5557,
    "peers": [{"name": "Beta", "endpoint": "tcp://10.19.54.177:5565"}],
    "subscriptions": [{"name": "Beta", "endpoint": "tcp://10.19.54.177:5567"}]
  }
}
```

Consumer 端同上，`dc_producer` 换 `dc_consumer` + `data_host`。

---

## 13. Module 运行循环

### 13.1 条件变量唤醒

```cpp
void Module::runLoop() {
    const int idleInterval = 100;
    int msgsSinceIdle = 0;
    while (running_) {
        cv_.wait_for(lock, 50ms, [this]{ return !queue_.empty() || !running_; });
        if (!queue_.empty()) {
            msg = queue_.front(); queue_.pop();
        }
        if (!msg.cmd.empty()) {
            handleState(msg);
            if (++msgsSinceIdle >= idleInterval)
                { handleState(Message{}); msgsSinceIdle = 0; }
        } else {
            handleState(Message{});
            msgsSinceIdle = 0;
        }
    }
}
```

| 策略 | 说明 |
|------|------|
| `post()` 时 `cv_.notify_one()` | 消息到来即时唤醒，不 idle 轮询 |
| `wait_for(50ms)` 兜底 | 无消息时心跳保活 |
| 每 100 条消息强制 idle | 保证 idle 帧不被持续消息饿死 |

idle 帧用于模块轮询内存变量（如 `action` 触发器）、退出检测等。

---

## 14. 与 MSM 对比

| 维度 | MSM | EPICS |
|------|-----|-------|
| 抽象 | Message (事件) | PV (Key-Value) |
| 通信 | 路由 (点对点/广播) | 订阅-推送 |
| 同步调用 | ✅ (-@) | ❌ |
| 批量传输 | ✅ | ❌ |
| 适用 | 控制/高速采集 | 监控 |

---

## 15. 下一步

1. ✅ Message 结构 + JSON 序列化
2. ✅ MessageQueue 实现
3. ✅ MessageBus 路由核心
4. ✅ 异步消息实现
5. ✅ 同步调用 + sendAndWait
6. ✅ 广播 + 订阅表
7. ✅ NI 字符串解析器
8. ✅ PVServer (in-process PVA + softIOC)
9. ✅ IPCServer 重构 (JSON驱动 + 自动发现)
10. ✅ MSMGateway (REP + DEALER + PUB + SUB) 跨进程 RPC + 广播
11. ✅ RESTServer (HTTP + 自动发现)
12. ✅ DataChannel (裸 TCP + ZMQ 控制面) 跨网络高吞吐
13. ✅ Module runLoop 条件变量唤醒 + idle 帧保证
14. 待定: MQTT Gateway 模块
