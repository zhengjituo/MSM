# MSM_DAQ 项目规范

## Workflow（重要）

**每次修改前先 git commit 备份当前状态**，以便随时回退。任何时候 AI 要修改源文件，人类应确认已经 commit 当前状态。

提交准则：
- 只提交源代码（`.cpp`/`.h`/`.py`/`CMakeLists.txt`/`config/*.json`/`docs/*.md`/`AGENTS.md`）
- 不提交构建产物（`build/`）、编译出的二进制（`main_ref_test`、`trigger3010`）、运行时文件（`runtime_config.json`、`.msm_session`）
- 提交信息简短描述做了什么

## 环境说明

- **工作目录**: `/mnt/hgfs/share/opencode/MSM`
- **构建系统**: CMake (最低版本 3.15)
- **C++ 标准**: C++17
- **依赖**: EPICS R7.0.8+ (libdbCore, libdbRecStd, libCom, libca) - 见下方 EPICS 获取说明
- **外部依赖**: 位于 `module_core/extern/` 下，`.gitignore` 已排除

## 目录结构

```
MSM/
├──AGENTS.md                    # 项目入口 (本文档)
├──tasks/                       # Task 工作日志 (YYYYMMDD_*.md)
├──docs/                        # 设计文档
│   └──design.md               # 核心设计规范
├──module_core/                 # 核心框架
│   ├──include/                # 公共头文件
│   ├──src/                    # 实现文件
│   ├──test/                   # 单元测试
│   ├──extern/                 # 外部依赖
│   │   ├──include/            # zmq.h, httplib.h
│   │   ├──lib/                # libzmq_minimal.a, libcpp-httplib.so
│   │   └──epics/              # EPICS base (需自行获取)
│   ├──CMakeLists.txt
│   └──AGENTS.md               # module_core 规范
├──module_calculator/          # 示例模块 (AddModule + MultiplyModule + WaveModule)
│   ├──AGENTS.md               # calculator 规范
│   ├──CMakeLists.txt
│   ├──msm_vars.json           # 变量配置
│   ├──msm_pvs.db             # EPICS 数据库文件 (自动生成)
│   ├──msm_dashboard.html      # Web GUI 仪表板
│   ├──generate_pydm_ui.py    # PyDM .ui 生成器
│   └──serve_gui.py           # Web GUI 开发服务器
└──.gitignore
```

## EPICS 外部依赖

项目依赖 EPICS R7.0.8+（带 CA 支持）。EPICS 目录 `module_core/extern/epics/` 已加入 `.gitignore`，需手动获取：

```bash
# 方式 1：从官方源码编译
#   下载 https://epics-controls.org/download/epics-base.tar.gz
#   解压后复制到 module_core/extern/epics/
#   参考 https://docs.epics-controls.org/ 编译说明

# 方式 2：从已有 EPICS 安装拷贝
cp -rL /path/to/your/epics-base module_core/extern/epics/

# 方式 3：升级覆盖
rm -rf module_core/extern/epics && cp -rL /path/to/new/epics-base module_core/extern/epics/
```

> 必须使用 `cp -rL`（解引用软链接），因为 `/mnt/hgfs/share/opencode/MSM` 为 VirtualBox 共享目录，不支持符号链接。

## 进入点与构建

```bash
# 构建
cd module_calculator
cmake -B build
cmake --build build

# 方法 A：编译太快超时（VirtualBox 共享目录慢）
# 改用本地 /tmp 手动编译（见下方说明）

# 运行 MSM Calculator (终端 1)
export LD_LIBRARY_PATH=../module_core/extern/lib:../module_core/extern/epics/lib/linux-x86_64
EPICS_CAS_INTF_ADDR_LIST=127.0.0.1 \
EPICS_CA_SERVER_PORT=5064 \
EPICS_CA_ADDR_LIST=127.0.0.1 \
EPICS_CA_AUTO_ADDR_LIST=NO \
./build/calculator_main

# 测试 CA (终端 2)
EPICS_CA_ADDR_LIST=127.0.0.1 caget MSM:AddModule:sum
EPICS_CA_ADDR_LIST=127.0.0.1 caput MSM:AddModule:a 10
EPICS_CA_ADDR_LIST=127.0.0.1 caput MSM:AddModule:b 20
EPICS_CA_ADDR_LIST=127.0.0.1 caput MSM:AddModule:action 1
EPICS_CA_ADDR_LIST=127.0.0.1 caget MSM:AddModule:sum  # → 30

# 测试 REST API (终端 3)
curl http://localhost:8080/api/AddModule/sum
curl -X PUT -d "10" http://localhost:8080/api/AddModule/a
curl -X PUT -d "20" http://localhost:8080/api/AddModule/b
curl -X PUT -d "1" http://localhost:8080/api/AddModule/action
curl http://localhost:8080/api/AddModule/sum  # → 30

# 测试 Web GUI (浏览器)
# 打开 http://localhost:8080/

# 测试 IPC (终端 4)
echo "PUT AddModule:a 10" | socat - UNIX-CONNECT:/tmp/msm.sock
echo "GET AddModule:sum" | socat - UNIX-CONNECT:/tmp/msm.sock
```

**注意**: 
- CA server 默认绑定 127.0.0.1:5064，客户端需设 `EPICS_CA_ADDR_LIST=127.0.0.1`
- 编译慢（httplib.h 为 header-only 大库），可拷贝到 `/tmp` 编译

---

## 核心概念

**消息驱动**: 模块间通过 `Message` 通信，不直接 import/header 依赖

**NI 字符串语法** (一等公民):
```
API >> args -@ Target    # 同步调用 (阻塞等待响应)
API >> args -> Target    # 异步有返回
API >> args ->| Target   # 异步无返回
API >> args -> <broadcast>  # 广播
```

**消息结构**:
```cpp
Message {
    cmd: "Calculate",
    data: {
        x: 10, y: 20,
        _sender: "ModuleA",
        _target: "ModuleB",
        _correlationId: 12345,
        _await: true,
        _mode: "call"
    }
}
```

## 开发流程

遵循 `task-with-review.yaml` 模式:

1. **Context Loading** - 读取 `docs/design.md`
2. **Requirements Clarification** - 参考 `tasks/requirements.md`
3. **Implementation** - 增量开发
4. **Testing** - 单元测试通过
5. **Commit** - 需明确授权

## 关键 API

### Module 基类 (`include/Module.h`)
- `Module(name)` - 构造函数传入模块名
- `start()` / `stop()` - 生命周期管理
- `post(Message)` / `post(NIString)` - 发送消息
- `handleState(Message)` - 消息处理入口
- `getVars()` - 变量接口 (CLI 必需)

### MessageBus (中央路由)
- `registerModule(Module*)` / `unregisterModule(name)`
- `subscribe(api, source, subscriber)` - 订阅
- `broadcast(Message)` - 广播

### IPCServer (外部消息注入)
- 基于 UNIX domain socket (`/tmp/msm.sock`)
- 接收 JSON 消息，注入到模块队列

### PVServer (EPICS 集成)
- 继承 Module，接收 `PV_UPDATE` 消息
- `enableIOC(epicsBase, dbPath)` - fork 独立 EPICS softIOC 子进程，提供 CA 协议
- PV_UPDATE 触发 `pushToIOC()` 通过 CA client 同步到 IOC record
- CA monitor 回调接收外部 `caput`，同步到 Module 内存
- `generateDB(path)` - 导出 EPICS `.db` 文件（`longout/ao` 可写，`longin/ai` 只读，`waveform` 支持数组）

### RESTServer (HTTP REST + Web GUI)
- 继承 Module，基于 cpp-httplib
- REST 端点：`GET/PUT /api/:module/:var`，`POST /api/:module/action`
- `enableStaticDir(path)` - 开启静态文件服务（Web GUI 仪表板）
- 数组变量支持（`double[]`，`int[]`）

### ZMQServer (消息级网络桥接)
- 继承 Module，ZMQ REQ/REP + PUB/SUB
- 支持 JSON 消息和 NI 字符串语法
- 默认端 `tcp://*:5555`

### IPCServer (本地调试)
- UNIX domain socket `/tmp/msm.sock`
- 变量级 GET/PUT/LIST/SCHEMA

## 设计约束

1. **解耦**: 模块间只通过模块名字符串寻址
2. **边缘智能**: 业务逻辑在边缘模块处理，不依赖中央
3. **消息类型区分**: 同步/异步/广播/订阅
4. **超时机制**: 支持 `timeout`、`interrupt`、`emergency_stop_`
5. **自动清理**: 模块退出时自动从 MessageBus 注销

## 调试提示

- 查看 `tasks/` 历史了解已完成功能
- `module_core/AGENTS.md` 提供核心框架细节
- `module_calculator/AGENTS.md` 提供 CLI 终端架构说明

## 与 EPICS 对比

| 维度 | EPICS | MSM |
|------|-------|-----|
| 抽象 | PV (Key-Value) | Message (事件) |
| 通信 | 订阅-推送 | 路由 (点对点/广播) |
| 同步调用 | ❌ | ✅ (`-@`) |
| 批量传输 | ❌ | ✅ |
| 适用 | 监控 | 控制/高速采集 |

详见 `MSMvsEPICS.md`

---

**最后更新**: 2026-06-07
