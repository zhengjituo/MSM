# MSM Core 升级计划：MSM_DAQ → module_core

## 目标

将远程 `10.19.54.177:/home/zkcx/ws/MSM_DAQ/` 中的旧 `msm/` 核心替换为本机 `module_core/`（新版 MSM），获得 EPICS、REST、ZMQ 扩展能力，同时保留 MSM_DAQ 的硬件采集/处理/存盘模块。

## API 差异分析

### 旧 `msm/`（远程）— C++14

| API | 位置 |
|-----|------|
| `Module::onIdle()` 虚方法 | `Module.h:30` |
| `Module::idleDelayMs_` (int, default 10) | `Module.h:45` |
| `runLoop`: `lock_guard` + `sleep_for(idleDelayMs_)` | `Module.cpp:20-35` |
| `post()`: 简单 push，无 `notify_one` | `Module.cpp:16-19` |
| `stop()`: 无 `cv_` 操作 | `Module.cpp:26-33` |
| `IPCServer::process()`: 含 `MSG` 命令转发 | `IPCServer.cpp:203-217` |
| `IPCServer::process()`: `PUT action` 自动 sleep 100ms | `IPCServer.cpp:279-282` |

### 新 `module_core/`（本机）— C++17

| API | 位置 |
|-----|------|
| ❌ **无** `onIdle()` — 空队列传 `handleState({})` | `Module.cpp:82` |
| ✅ `cv_` + `condition_variable` — 50ms wait_for | `Module.cpp:67` |
| ✅ `post()` 含 `cv_.notify_one()` | `Module.cpp:40` |
| ✅ `stop()` 含 `cv_.notify_one()` | `Module.cpp:28` |
| ❌ **无** `MSG` 命令 | `IPCServer.cpp:190-288` |
| ✅ `post(NIString)` NI 字符串重载 | `Module.cpp:43-57` |
| ✅ `MessageBus::sendAndWait()` 远程同步调用 | `MessageBus.h:27` |
| ✅ `PVServer`: EPICS CA server | `PVServer.h` |
| ✅ `RESTServer`: HTTP REST + Web GUI | `RESTServer.h` |
| ✅ `MSMGateway`: ZMQ 网关 | `MSMGateway.h` |

## 升级步骤

### Step 1：修补 `module_core/` 的向后兼容性

在 **本机** 上修改 `module_core/`，补上 MSM_DAQ 需要但缺失的 API：

#### 1a. `Module.h` 加 `onIdle()` + `idleDelayMs_`

```cpp
// 加到 Module class protected: 区域
virtual void onIdle() {}
int idleDelayMs_ = 10;
```

#### 1b. `Module.cpp` 改 `runLoop()` — 融合新旧模式

```
while running_:
  1. unique_lock + cv_.wait_for(50ms)         ← 快唤醒不走
  2. if queue not empty:
       msg = pop + handleState(msg)
       if ++msgsSinceIdle >= 100:
         msg = {}; msgsSinceIdle = 0
  3. else:
       onIdle()                                ← 新加：空队列时调 onIdle()
       msgsSinceIdle = 0
       (不 sleep，cv_.wait_for 已兜底)
```

这样既保留了 `cv_` 的快唤醒（`post` 后立即处理），又兼容了 MSM_DAQ 的 `onIdle()` 模式。

#### 1c. `IPCServer.cpp` 加 `MSG` 命令

把旧 MSM 的 MSG 转发逻辑加上：
```cpp
// process() 开头加：
if (line starts with "MSG ") {
    niStr = line.substr(4);
    trim(niStr);
    msg = NIStringParser::parse(niStr, "IPCServer");
    MessageBus::instance().send(msg);
    return "{\"OK\":\"sent\"}";
}
```
同时加 `PUT action` 的 100ms sleep。

### Step 2：使 EPICS/ZMQ/httplib 为可选依赖

`module_core/CMakeLists.txt` 加编译选项：

```cmake
option(MSM_ENABLE_EPICS "Enable EPICS CA support" ON)
option(MSM_ENABLE_ZMQ  "Enable ZMQ gateway" ON)
option(MSM_ENABLE_HTTP "Enable REST/Web server" ON)

# 只有 ON 时才编译 PVServer/RESTServer/MSMGateway
# 只有 ON 时链接 EPICS/ZMQ/httplib
```

MSM_DAQ 编译时：
```bash
cmake -B build -DMSM_ENABLE_EPICS=OFF -DMSM_ENABLE_ZMQ=OFF -DMSM_ENABLE_HTTP=OFF
```

### Step 3：改造 MSM_DAQ 的 CMakeLists.txt

```
MSM_DAQ/
├── module_core/       ← 替换旧 msm/ (从本机复制)
├── module_daq/        ← 不变
├── module_process/    ← 不变
├── module_storage/    ← 不变
├── module_sim/        ← 不变
├── Module_main/       ← 不变
├── extern/
│   └── circular_buffer/  ← 不变
├── sdk_include/       ← 不变
└── CMakeLists.txt     ← 重写
```

新的 `CMakeLists.txt` 结构：

```cmake
cmake_minimum_required(VERSION 3.15)
project(MSM_DAQ LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(module_core)   # 编译 msm_core 静态库

add_library(module_daq STATIC ...)
target_link_libraries(module_daq msm_core ...)

add_executable(msm_daq_storage ...)
target_link_libraries(msm_daq_storage module_daq module_process module_storage msm_core)
```

### Step 4：文件复制到远程

```bash
# 本机
scp -r module_core/ zkcx@10.19.54.177:/home/zkcx/ws/MSM_DAQ/

# 覆盖旧 msm/ 目录
ssh zkcx@10.19.54.177 "rm -rf /home/zkcx/ws/MSM_DAQ/msm"
ssh zkcx@10.19.54.177 "mv /home/zkcx/ws/MSM_DAQ/module_core /home/zkcx/ws/MSM_DAQ/msm"

# 拷 nlohmann/json.hpp 到 msm/include/ (新 core 的 extern/include 里有)
```

或者保留为 `module_core/` 并更新 CMakeLists.txt 引用路径。

### Step 5：远程构建和验证

```bash
cd /home/zkcx/ws/MSM_DAQ
rm -rf build
cmake -B build -DMSM_ENABLE_EPICS=OFF -DMSM_ENABLE_ZMQ=OFF -DMSM_ENABLE_HTTP=OFF
cmake --build build
```

然后执行 `msm_daq_sim` 验证 sim 模式正常。

### Step 6：扩展（后续）

升级成功后可按需开启：
- `MSM_ENABLE_EPICS=ON` — EPICS CA 集成（需 EPICS base）
- `MSM_ENABLE_ZMQ=ON` — ZMQ 网关（需 libzmq_minimal.a）
- `MSM_ENABLE_HTTP=ON` — REST API + Web GUI（需 libcpp-httplib.so）
- `DataChannel` — TCP 高吞吐传输（从本机 `module_datachannel/` 复制）

## 文件变更清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `module_core/include/Module.h` | 修改 | 加 `onIdle()` + `idleDelayMs_` |
| `module_core/src/Module.cpp` | 修改 | `runLoop` 融合 cv + onIdle |
| `module_core/src/IPCServer.cpp` | 修改 | 加 MSG 命令 + PUT action sleep |
| `module_core/CMakeLists.txt` | 修改 | 加 MSM_ENABLE_* 选项 |
| `MSM_DAQ/CMakeLists.txt` | 重写 | 引用 module_core，去旧 msm |
| `MSM_DAQ/msm/` | 删除 | 旧核心框架 |
| `MSM_DAQ/AGENTS.md` | 修改 | 更新构建说明 |

## 风险

1. **`idleDelayMs_` 行为变化**: 旧版 onIdle 时 sleep 10ms，新版 cv 50ms wait_for，响应延迟可能有差异。MSM_DAQ 的 `doReadOnce()` 在 onIdle 里调用，采集循环可能比原来快约 5 倍（10ms → 2ms 实际唤醒间隔），CPU 占用略升
2. **`post()` 不加锁差异**: 旧版无 cv，新版有 `cv_.notify_one()`，对调用方无影响
3. **EPICS/ZMQ/httplib 不存在时**: 设为 OFF 即不编译不链接，无风险

## 交接给远程 Opencode

复制以下文件到远程后，对方 opencode 先读：
1. `tasks/20260618_msm_core_upgrade_plan.md` — 本文档
2. `module_core/AGENTS.md` — 新 core 规范
3. `AGENTS.md` — 更新后的项目规范

然后依次执行 Step 4 → Step 5 验证。
