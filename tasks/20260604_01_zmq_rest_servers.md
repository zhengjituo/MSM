# ZMQServer + RESTServer 实现

## 目标

实现两个新的接口服务 Module：
1. **ZMQServer** — ZMQ REQ/REP + PUB/SUB 桥接 MessageBus，支持远程消息级通信
2. **RESTServer** — HTTP REST + WebSocket，变量级 GET/PUT + 消息级 POST

## 架构

两者都继承 `Module`，注册到 `MessageBus`。与已有的 IPCServer、PVServer 组成统一接口层。

- ZMQServer: 消息级，对接 MSM 原生客户端，支持 NI 字符串
- RESTServer: 变量级 + 消息级，对接 Web/第三方，支持 JSON

## 文件变更

| 文件 | 动作 |
|------|------|
| `module_core/include/ZMQServer.h` | 新增 |
| `module_core/src/ZMQServer.cpp` | 新增 |
| `module_core/include/RESTServer.h` | 新增 |
| `module_core/src/RESTServer.cpp` | 新增 |
| `module_core/CMakeLists.txt` | 修改 (添加 zmq + httplib 链接) |
| `module_calculator/src/CalculatorMain.cpp` | 修改 (启用 ZMQ + REST) |
| `docs/design.md` | 修改 (添加接口服务层文档) |

## 验收

1. ZMQ 客户端发送 NI 字符串 → 模块计算 → 返回结果
2. HTTP GET /api/AddModule/sum 返回当前值
3. HTTP PUT /api/AddModule/a 10 写入变量
4. HTTP POST /api/AddModule/action {"action":1} 触发计算
5. WebSocket 推送变量变更
6. 全部测试通过后 commit
