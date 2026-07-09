# MSM 使用指南

## 快速开始

```bash
# 设置 EPICS 环境
export EPICS_BASE=/home/et/EPICS/epics-base
export EPICS_HOST_ARCH=linux-x86_64
export LD_LIBRARY_PATH=$EPICS_BASE/lib/$EPICS_HOST_ARCH

# 构建
cd module_calculator
cmake -B build
cmake --build build

# 运行
./build/calculator_main
```

启动后自动加载 `msm_vars.json`，启动四个接口服务：

```
MSM Calculator - All Interfaces Ready
========================================
  Config: msm_vars.json
  IPC:    /tmp/msm.sock
  PVA:    MSM:*
  ZMQ:    tcp://*:5555
  REST:   http://localhost:8080/api
```

---

## 接口使用

### 1. IPC (UNIX Domain Socket)

用于本地进程间通信，逐变量读写。

```bash
# 写入
echo "PUT AddModule:a 10" | socat - UNIX-CONNECT:/tmp/msm.sock
echo "PUT AddModule:b 20" | socat - UNIX-CONNECT:/tmp/msm.sock
echo "PUT AddModule:action 1" | socat - UNIX-CONNECT:/tmp/msm.sock

# 读取
echo "GET AddModule:sum" | socat - UNIX-CONNECT:/tmp/msm.sock
# → {"OK":30}

# 列表
echo "LIST" | socat - UNIX-CONNECT:/tmp/msm.sock

# 结构
echo "SCHEMA AddModule:*" | socat - UNIX-CONNECT:/tmp/msm.sock
```

**Python 客户端**:
```python
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('/tmp/msm.sock')
s.send(b'PUT AddModule:a 10\n')
print(s.recv(4096).decode())
s.close()
```

---

### 2. PVA (EPICS PV Access)

原生 EPICS PVA 协议，EPICS 用户直接使用标准工具。

```bash
# 写入
pvput MSM:AddModule:a 10
pvput MSM:AddModule:b 20
pvput MSM:AddModule:action 1

# 读取
pvget MSM:AddModule:sum
# → 30

# 监控
pvmonitor MSM:AddModule:sum
```

PV 命名规则: `{prefix}:{module}:{var}`（prefix 在 `msm_vars.json` 中配置）。

---

### 3. ZMQ (tcp://\*:5555)

用于远程 MSM 原生客户端，支持 JSON 和 NI 字符串两种格式。

#### JSON 格式

```python
import zmq
ctx = zmq.Context()
s = ctx.socket(zmq.REQ)
s.connect("tcp://server:5555")

# 原子操作：一次调用完成写入 + 计算 + 读取
s.send_json({
    "cmd": "Calculate",
    "data": {"a": 10, "b": 20},
    "_target": "AddModule"
})
resp = s.recv_json()
print(resp["variables"])
# → {"a": "10", "action": "0", "b": "20", "sum": "30"}
```

#### NI 字符串格式

```
Calculate >> a=10,b=20 -@ AddModule
```

#### 内置命令

```python
# LIST — 列出所有变量当前值
s.send_string("LIST")
print(s.recv_json())
# → {"AddModule": {"a": "0", "b": "0", "sum": "0", "action": "0"}, ...}

# SCHEMA — 列出所有变量元数据
s.send_string("SCHEMA")
print(s.recv_json())
```

#### C++ 客户端

```cpp
#include <zmq.h>
#include <string>
#include <cstring>

void *ctx = zmq_ctx_new();
void *sock = zmq_socket(ctx, ZMQ_REQ);
zmq_connect(sock, "tcp://localhost:5555");

const char *req = R"({"cmd":"Calculate","data":{"a":10,"b":20},"_target":"AddModule"})";
zmq_send(sock, req, strlen(req), 0);

char buf[4096];
int n = zmq_recv(sock, buf, sizeof(buf)-1, 0);
buf[n] = 0;
// buf → {"cmd":"Calculate","status":"ok","variables":{...}}
```

---

### 4. REST (http://localhost:8080)

HTTP REST API，适合 Web 前端、curl、PYDM 等第三方集成。

#### 读取变量

```bash
curl http://localhost:8080/api/AddModule/sum
# → {"ok":"30"}
```

#### 读取模块全部变量

```bash
curl http://localhost:8080/api/AddModule
# → {"module":"AddModule","variables":{"a":"10","action":"0","b":"20","sum":"30"}}
```

#### 写入变量

```bash
curl -X PUT -d '10' http://localhost:8080/api/AddModule/a
# → {"ok":"10"}
```

#### 触发计算 (原子操作)

```bash
curl -X POST -d '{"action":"1"}' http://localhost:8080/api/AddModule/action
# → {"module":"AddModule","status":"ok","variables":{"sum":"30"}}
```

#### 获取 Schema

```bash
curl http://localhost:8080/api/schema          # 所有模块
curl http://localhost:8080/api/AddModule/schema  # 单个模块
```

#### Python 客户端

```python
import requests
r = requests.get("http://localhost:8080/api/AddModule/sum")
print(r.json())  # {"ok": "30"}
r = requests.put("http://localhost:8080/api/AddModule/a", data="10")
r = requests.post("http://localhost:8080/api/AddModule/action",
                  json={"action": "1"})
```

---

## 配置 (msm_vars.json)

```json
{
    "prefix": "MSM",
    "snapshot_interval": 5,
    "modules": [
        {
            "name": "AddModule",
            "memory_size": 16,
            "variables": [
                {"name":"a",      "type":"int", "offset":0,  "len":4, "dir":"i",  "epics":1},
                {"name":"b",      "type":"int", "offset":4,  "len":4, "dir":"i",  "epics":1},
                {"name":"sum",    "type":"int", "offset":8,  "len":4, "dir":"o",  "epics":1},
                {"name":"action", "type":"int", "offset":12, "len":4, "dir":"i",  "epics":1}
            ]
        }
    ]
}
```

| 字段 | 说明 |
|------|------|
| `prefix` | PV 名前缀 (`MSM:AddModule:sum`) |
| `snapshot_interval` | 快照保存间隔(秒)，0 关闭 |
| `memory_size` | 模块内存结构体大小(字节)，对应 C++ struct |
| `name` | 变量名，对应 struct 字段名 |
| `type` | 类型: `int`, `double`, `string`, `double[]` |
| `offset` | 结构体中的字节偏移 |
| `len` | 字节长度 |
| `dir` | 方向: `i`(输入), `o`(输出), `io`(双向) |
| `epics` | 是否导出为 EPICS PV (`1`/`0`) |

**添加新模块只需**: C++ struct + JSON 配置 + `setModuleMemory()`，零改动接口服务层。

---

## 架构

```
msm_vars.json ──→ 内存偏移描述
                      │
模块内存 (共享指针) ──┤
                      │
          ┌──────┬──────┬──────┬──────┐
          ▼      ▼      ▼      ▼      ▼
      IPCServer PVServer ZMQServer RESTServer
      UNIX Skt  PVA     ZMQ TCP   HTTP

所有接口服务 → Module 子类 → 注册到 MessageBus
```

每个接口服务都是独立的 `Module` 子类，可独立开关。
