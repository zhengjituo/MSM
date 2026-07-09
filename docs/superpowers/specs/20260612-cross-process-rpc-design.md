# Cross-Process RPC via MSMGateway

## Goal

Two processes simulating two industrial hosts, each with a local compute module, communicating via MSMGateway over ZMQ. Process A asks B to multiply, B asks A to add — each module sees remote modules as transparent proxies.

## Architecture

```
Process Alpha                          Process Beta
┌─────────────────────┐               ┌─────────────────────┐
│ MessageBus          │               │ MessageBus          │
│  AddModule (real)   │               │  MultiplyModule(real)│
│  MSMGateway         │               │  MSMGateway         │
│  REP   tcp://*:5555 │◄─REP/DEALER───┤  REP   tcp://*:5556 │
│  DEALER→ tcp://B:5556│──DEALER/REP─►│  DEALER→ tcp://A:5555│
│  PUB   tcp://*:5557 │──PUB─────────►│  SUB   tcp://A:5557  │
└─────────────────────┘               └─────────────────────┘
```

### ZMQ Socket Roles

| Socket | Role | Usage |
|--------|------|-------|
| REP  | 多路入口 | 接收任何设备/客户端发来的请求，处理后原路响应 |
| DEALER | 定点发送口 | 发送请求到指定对端，异步收响应 |
| PUB | 被订阅广播口 | 广播消息，所有订阅者接收 |

## Message Protocol

### _remote 标记

Message 新增 `_remote` 字段，值为对端 peer 名。MessageBus 见到此字段直接转给 MSMGateway，不走本地查找。

### 请求/响应 JSON

```
REQ → REP (或 DEALER → REP):
{
  "cmd": "Calculate",
  "_target": "MultiplyModule",
  "_sender": "AddModule",
  "_await": true,
  "_correlationId": 12345,
  "data": { "a": "3", "b": "4" }
}

Response ←:
{
  "type": "response",
  "cmd": "Response",
  "_correlationId": 12345,
  "data": { "result": "12" }
}
```

## Components

### 1. Message — `_remote` 字段

```cpp
void setRemote(const std::string& peer);
std::string getRemote() const;    // data.get("_remote")
bool isRemote() const;            // !getRemote().empty()
```

### 2. MessageBus — _remote 路由

`send()` / `sendAndWait()` 入口处判断：
- `_remote` 非空 → 转给 `MSMGateway`（用 `gw->post(msg)`）
- 空 → 走原本地逻辑

`send()` 找不到目标模块时不直接丢弃，给网关拦截机会。

### 3. MSMGateway — REP + DEALER + PUB

**REP** 线程:
- `recv JSON` → `Message`
- `_await=true` → `sendAndWait` 等本地模块 → 结果回给 DEALER
- `_await=false` → `send` 投递 → 回 `{"status":"ok"}`

**DEALER** 线程:
- 轮询收对端 REP 返回的 `type=response`
- 通过 correlationId 找到本地 promise → `notifyResponse`

**PUB** 线程:
- 订阅 MessageBus 上的 broadcast 消息 → 转发到 ZMQ PUB

**静态路由表**: `peerName → { repEndpoint }`，启动时配置，不动态变化。

### 4. AddModule — 新增变量

| 变量 | 类型 | 说明 |
|------|------|------|
| `callRemote` | int | 按钮，设1时发送远程乘法请求 |
| `remoteResult` | int | 只读，对端返回的乘法结果 |

### 5. MultiplyModule — 新增变量

| 变量 | 类型 | 说明 |
|------|------|------|
| `callRemote` | int | 按钮，设1时发送远程加法请求 |
| `remoteResult` | int | 只读，对端返回的加法结果 |

## External ZMQ Client

任意标准 ZMQ REQ socket 连接 REP port:

```python
import zmq, json
ctx = zmq.Context()
s = ctx.socket(zmq.REQ)
s.connect("tcp://192.168.1.100:5555")
s.send(json.dumps({
    "cmd": "Calculate", "_target": "MultiplyModule",
    "_sender": "PyClient", "_await": True,
    "_correlationId": 1, "data": {"a":"3","b":"4"}
}))
resp = json.loads(s.recv())
print(resp["data"]["result"])  # "12"
```

## Programs

```bash
calc_alpha --rep-port 5555 --pub-port 5557 \
  --peer Beta,tcp://localhost:5556

calc_beta --rep-port 5556 --pub-port 5558 \
  --peer Alpha,tcp://localhost:5555
```

Each program registers its local module, starts MSMGateway with static peer config, then runs a demo loop that exercises both local and remote calculation.
