# MSM EPICS IOC 扩展设计

> **目标**: 让 EPICS 客户端 (caget/caput/ca_monitor) 能操作 MSM ModuleVars

## 架构

```
┌─────────────────────────────────────────────────────────────┐
│              MSM Calculator 进程                              │
│                                                              │
│  ModuleVars (统一接口)                                        │
│  ├─ get(name) → string                                       │
│  ├─ set(name, string) → bool                                 │
│  └─ list() → map<string,string>                              │
│              │                                                │
│      ┌───────┴───────┐                                       │
│      ▼               ▼                                       │
│ IPCServer        EPICS IOC                                   │
│ Thread           Thread                                     │
│ (Socket)         (CAS)                                      │
│      │               │                                       │
│      │          ┌────▼────┐                                  │
│      │          │ caget/ │                                  │
│      │          │ caput  │                                  │
│      │          └─────────┘                                  │
└──────┴──────────────────────────────────────────────────────┘
```

### 核心设计原则

1. **MSM 是主人** - EPICS IOC 是 MSM 的扩展，不是核心
2. **直接访问** - EPICS IOC 线程直接读写 ModuleVars，不通过 IPCServer
3. **透传** - EPICS IOC 只做协议转换，不含业务逻辑
4. **松耦合** - EPICS IOC 可禁用，禁用后 MSM 核心不变

## SCHEMA JSON

### 格式

```json
{
  "version": "1.0",
  "prefix": "MSM",
  "modules": {
    "AddModule": {
      "vars": [
        {"name": "a", "type": "double", "count": 1},
        {"name": "b", "type": "double", "count": 1},
        {"name": "sum", "type": "double", "count": 1}
      ]
    },
    "WaveModule": {
      "vars": [
        {"name": "wave", "type": "double", "count": 10},
        {"name": "size", "type": "int", "count": 1}
      ]
    }
  }
}
```

### 生成

```cpp
// ModuleVars 子类实现 toSchema()
Json::Value ModuleVars::toSchema() {
    Json::Value m;
    m["name"] = moduleName_;
    m["vars"] = Json::array();
    for (auto& v : vars_) {
        m["vars"].append({{"name", v.name}, {"type", v.type}, {"count", v.count}});
    }
    return m;
}
```

## EPICS IOC 线程

### 接口

```cpp
class EPICSServer {
public:
    EPICSServer(const std::string& schemaPath);
    
    void registerModule(const std::string& name, Module* mod);
    void start(int tcpPort = 5064);
    void stop();
    
    // 内部调用
    void notifyValueChanged(const std::string& module, const std::string& var);
    
private:
    void runTCPLoop();
    // EPICS CAS 回调
    static void searchCallback(const char* name, void* arg);
    static void connectionCallback(...);
    
    std::string prefix_;
    std::map<std::string, Module*> modules_;  // name → Module
    std::atomic<bool> running_;
    std::thread thread_;
};
```

### PV 命名

```
{prefix}:{moduleName}:{varName}

MSM:ADD:a        → AddModule.a
MSM:WAVE:wave    → WaveModule.wave[10]
```

### 类型映射

| Schema type | EPICS DBR | 说明 |
|-------------|-----------|------|
| int | DBR_LONG | 32位整数 |
| double | DBR_DOUBLE | 64位浮点 |
| waveform[N] | DBR_WAVEFORM | 数组 |

### 读写流程

**GET (caget)**:
```
EPICS 客户端 → TCP Read Request
    ↓
EPICSServer.searchCallback() 查找 PV
    ↓
EPICSServer.readRequest() 调用 ModuleVars.get(name)
    ↓
返回 value 给客户端
```

**PUT (caput)**:
```
EPICS 客户端 → TCP Write Request
    ↓
EPICSServer.writeRequest() 调用 ModuleVars.set(name, value)
    ↓
ModuleVars 内部解析并设置
    ↓
EPICSServer.notifyValueChanged() 更新 monitor
    ↓
返回 status 给客户端
```

**MONITOR (ca_monitor)**:
```
客户端订阅 "MSM:WAVE:wave"
    ↓
EPICSServer 创建 monitor entry
    ↓
值变化时，ModuleVars.set() 调用 notifyValueChanged()
    ↓
推送更新给订阅者
```

## 实现阶段

### Phase 1: 基础 (当前)
- [ ] SCHEMA JSON 生成接口
- [ ] EPICSServer 基础框架
- [ ] 标量读写 (int, double)
- [ ] 基本测试 (caget/caput)

### Phase 2: 数组支持
- [ ] waveform 类型
- [ ] monitor 功能

### Phase 3: 增强
- [ ] 动态 PV 注册
- [ ] alarm/severity 简化实现

## 文件结构

```
module_core/
├── include/
│   ├── EPICSServer.h       # 新增
│   └── ModuleVars.h        # 修改: 增加 toSchema()
├── src/
│   ├── EPICSServer.cpp     # 新增
│   └── Module.cpp          # 修改: 集成 EPICS IOC
└── test/
    └── testEPICS.cpp       # 新增

docs/
└── superpowers/
    └── specs/
        └── 2025-05-15-msm-epics-design.md  # 本文档
```

## 依赖

- EPICS Base (用于 CAS 库)
- 路径: `/home/et/EPICS/epics-base`

## 测试

```bash
# 终端 1: 启动 MSM Calculator
cd module_calculator/build && ./calculator_main

# 终端 2: EPICS 客户端测试
caget MSM:ADD:a        # 读取
caput MSM:ADD:a 10     # 写入
camonitor MSM:WAVE:wave  # 监控
```

---

## 与 IPCServer 对比

| 方面 | IPCServer | EPICSServer |
|------|-----------|-------------|
| 协议 | 自定义 Socket | EPICS CA |
| 客户端 | socat/Python | caget/caput |
| 用途 | 开发调试 | 接入 EPICS 生态 |
| PV 格式 | Module:var | {prefix}:Module:var |
| 类型 | 动态 | Schema 确定 |

---

## 参考

- EPICS Channel Access Specification
- EPICS Base CAS (Channel Access Server) 接口