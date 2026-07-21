# MSM vs EPICS + Bluesky 深度对比

## EPICS 生态全景

```
┌─────────────────────────────────────────────────────────────────┐
│                        EPICS 生态                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   ┌──────────────┐    ┌──────────────┐    ┌──────────────┐    │
│   │   PyDM       │    │   Bluesky    │    │ StreamDevice│    │
│   │  (Qt GUI)    │    │ (扫描控制)   │    │(高速设备)   │    │
│   └──────┬───────┘    └──────┬───────┘    └──────┬───────┘    │
│          │                   │                   │            │
│          └───────────┬───────┴───────────────────┘            │
│                      ▼                                        │
│          ┌─────────────────────────┐                          │
│          │    IOC Server          │                          │
│          │  - 数据库记录           │                          │
│          │  - 回调处理             │                          │
│          │  - 设备驱动             │                          │
│          └───────────┬─────────────┘                          │
│                      ▼                                        │
│          ┌─────────────────────────┐                          │
│          │   Channel Access (CA)  │                          │
│          └───────────┬─────────────┘                          │
│                      ▼                                        │
│          ┌─────────────────────────┐                          │
│          │   Hardware / Network   │                          │
│          └─────────────────────────┘                          │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## StreamDevice (高速设备)

```cpp
// StreamDevice 方式
protocol myDevice {
    out "INIT";      # 发送
    in  "ACK";       # 接收
    out "DATA=%d";   # 带参数
    in  "READY";
}
```

- 针对串口、GPIB、网络等异步设备
- 协议解析，但仍然是**单次请求-响应**
- 适合 kHz 级设备控制，不适合 MHz 级数据采集

---

## IOC 回调事件编程

```c
// EPICS 记录回调
static long myInputRecord(DBF_LONG *prec) {
    // 读取硬件
    prec->val = readHardware();
    return 0;
}

static long myAlarmCallback(DBF_LONG *prec) {
    // 报警处理
    if (prec->val > threshold) {
        callCallback(prec);
    }
    return 0;
}
```

- 基于记录的事件处理
- 每个记录独立，无法形成完整流程
- 不如 MSM 的 handleState 状态机灵活

---

## Bluesky (实验流程控制)

```python
# Bluesky 扫描示例
from bluesky import RunEngine
from bluesky.plans import scan, grid_scan

RE = RunEngine({})

# 1D 扫描
RE(scan(det, motor, 0, 10, 20))

# 2D 网格扫描
RE(grid_scan(det, motor1, 0, 10, 5, motor2, 0, 5, 10))

# 自定义流程
def myExperiment():
    yield from mv(motor1, 0)
    yield from mv(motor2, 0)
    yield from scan(det, motor, 0, 10, 20)
    yield from mv(motor1, 5)
```

**Bluesky 特点：**
- Python 协程 (generator-based)
- 扫描抽象 (1D, 2D, 3D, 螺旋...)
- 实验队列管理
- 数据归档 (DataBroker)

---

## 对比总览

| 维度 | EPICS + Bluesky | MSM + TS/Go |
|------|-----------------|-------------|
| **GUI** | PyDM (Qt) | Web (TS/HTML) |
| **控制层** | Bluesky (Python) | TS/Go 应用层 |
| **核心层** | IOC (C/C++) | MSM Core (C++) |
| **状态机** | Python 协程 | C++ handleState |
| **实时性** | Python (慢) | C++ (快) |
| **硬件驱动** | 成熟驱动库 | 需要适配 |
| **生态** | 成熟 (30年) | 新生态 |

---

## MSM 优势

### 1. 实时性能 (关键区别)

```
Bluesky (Python):
  for point in scan():
      time.sleep(0.001)  # Python 调度开销
      → 毫秒级延迟

MSM (C++):
  while (running_) {
      Message msg = queue.pop();
      handleState(msg);  // C++ 直接执行
      → 微秒级延迟
  }
```

**场景**：MHz 级数据采集，Bluesky 跟不上

### 2. 状态机在核心层

```cpp
// MSM - 状态机在 C++ 核心
class ScopeModule : public Module {
    void handleState(const Message& msg) override {
        switch (state_) {
            case IDLE:
                if (msg.cmd == "Start") {
                    state_ = ACQUIRING;
                    startAcquisition();
                }
                break;
            case ACQUIRING:
                if (msg.cmd == "WaveData") {
                    processWaveform(msg.data);
                    // 实时处理，无 Python 开销
                }
                break;
        }
    }
};
```

```python
# Bluesky - 状态在 Python
def myPlan():
    state = "idle"
    yield from mv(motor, 0)
    for i in range(100):
        yield from trigger(det)  # 每次都跨语言
```

### 3. 批量数据处理

```cpp
// MSM - 批量消息
Message msg;
msg.data.set("wave", vector<float>(10000)); // 1万点一次

// 直接在 C++ 处理，无跨语言开销
```

```python
# Bluesky - 逐点处理
for i in range(10000):
    yield from trigger(det)  # 1万次跨语言调用
```

### 4. 现代 Web 前端

```
PyDM:        Qt 桌面应用 (绑定 Windows/Linux)
MSM Web UI:  浏览器运行 (跨平台，AI 辅助开发)
```

---

## Bluesky 优势

| 方面 | 说明 |
|------|------|
| **扫描抽象** | grid_scan, spiral_scan, adaptive_scan 等现成 |
| **数据归档** | DataBroker 自动保存 + HDF5 |
| **生态成熟** | 验证于全球几十个实验室 |
| **Python 生态** | numpy, scipy, scikit-learn 集成 |
| **社区支持** | 大型用户群，文档丰富 |

---

## 适用场景对比

| 场景 | EPICS + Bluesky | MSM + TS/Go |
|------|-----------------|-------------|
| 高频采集 (MHz+) | ❌ Python 瓶颈 | ✅ C++ 核心 |
| 复杂流程控制 | ✅ 成熟框架 | ⚠️ 需自建 |
| 状态机实时响应 | ⚠️ Python 调度 | ✅ C++ 即时 |
| 简单参数监控 | ✅ EPICS 擅长 | ❌ 过度设计 |
| Web 跨平台 UI | ❌ PyDM 局限 | ✅ HTML/TS |
| AI 辅助开发 UI | ❌ | ✅ TypeScript |
| 已有 EPICS 设备 | ✅ 直接集成 | ⚠️ 需适配 |

---

## 结论

**MSM 存在价值：**

1. **实时性**：C++ 核心适合高速采集、硬实时控制
2. **现代架构**：Web 前端 + 消息状态机，更灵活
3. **AI 友好**：TypeScript + HTML，AI Agent 生成代码方便

**不适合场景：**

- 已有成熟 EPICS 生态的实验室
- 需要现成扫描框架 (Bluesky 已实现)
- 简单监控级应用

**最佳策略：**

```
┌─────────────────────────────────────────┐
│         大科学控制系统架构              │
├─────────────────────────────────────────┤
│                                         │
│   高速采集 + 实时控制 → MSM (C++)       │
│   扫描 + 数据归档 → Bluesky (Python)    │
│   或者                                  │
│   全部 → MSM (C++) + Web               │
│                                         │
└─────────────────────────────────────────┘
```

未来：随着 Web 技术发展，MSM + Web 可能会逐步替代部分 EPICS + PyDM 场景。