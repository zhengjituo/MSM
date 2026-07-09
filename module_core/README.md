# MSM (Message-based State Machine) 框架

基于 NI CSM (Communicable State Machine) 的消息驱动状态机框架，C++ 实现。

## 目录结构

```
module_core/
├── include/          # 头文件
│   ├── Message.h
│   ├── MessageQueue.h
│   ├── NIStringParser.h
│   ├── Module.h
│   └── MessageBus.h
├── src/              # 源文件
├── test/             # 测试用例
├── CMakeLists.txt    # 构建配置
└── build/            # 构建输出
```

## 快速开始

### 1. 克隆并构建

```bash
cd module_core
cmake -B build -G "Visual Studio 17 2022"
cmake --build build
```

### 2. 创建自定义模块

```cpp
#include "Module.h"
#include "MessageBus.h"
#include <iostream>

using namespace msm;

class MyModule : public Module {
public:
    MyModule(const std::string& name) : Module(name) {}
    
    // 实现 handleState 处理消息
    void handleState(const Message& msg) override {
        if (msg.cmd == "Hello") {
            std::cout << getName() << " received Hello" << std::endl;
            
            // 发送响应
            Message resp;
            resp.cmd = "Response";
            resp.setCorrelationId(msg.getCorrelationId());
            resp.setSender(getName());
            resp.setTarget(msg.getSender());
            post(resp);
        }
    }
};

int main() {
    MyModule A("ModuleA"), B("ModuleB");
    
    // 注册模块
    MessageBus::instance().registerModule(&A);
    MessageBus::instance().registerModule(&B);
    
    // 启动模块
    A.start();
    B.start();
    
    // 发送消息
    B.post("Hello >> -@ ModuleA");  // 同步调用
    // B.post("Hello >> -> ModuleA"); // 异步调用
    
    // 等待处理
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 停止模块
    A.stop();
    B.stop();
    
    return 0;
}
```

## API 说明

### Module 类

| 方法 | 说明 |
|------|------|
| `Module(name, max_queue_size, idle_wait_time)` | 构造函数 |
| `start()` | 启动消息循环 |
| `stop()` | 停止并清理订阅 |
| `post(Message)` | 发送消息（自动路由） |
| `post(NIString)` | 发送 NI 格式字符串 |
| `setDryRun(bool)` | 开启测试模式 |
| `isDryRun()` | 检查是否测试模式 |

### 消息格式

**NI 字符串语法**:
```
API >> args -@ Target    # 同步调用 (阻塞等待响应)
API >> args -> Target    # 异步调用 (不阻塞，等待响应)
API >> args ->| Target   # 异步无返回
API >> args -> <broadcast>  # 广播
```

**Message 结构**:
```cpp
Message msg;
msg.cmd = "API";                      // 命令
msg.data.set("key", "value");        // 数据
msg.setTarget("ModuleA");            // 目标
msg.setSender("ModuleB");             // 发送者
msg.setCorrelationId(12345);          // 关联ID
msg.setAwaitResponse(true);          // 是否等待响应
```

### _ 前缀字段 (框架内部)

| 字段 | 说明 |
|------|------|
| `_sender` | 发送方模块名 |
| `_target` | 目标模块名 |
| `_correlationId` | 关联ID，用于响应路由 |
| `_await` | 是否等待响应 |
| `_timeout` | 超时毫秒数 |
| `_mode` | call/async/broadcast |

## 特性

- **同步调用**: `-@` 符号，阻塞等待响应
- **异步调用**: `->` 符号，不阻塞，等待异步响应
- **广播订阅**: `<broadcast>` / `<register>` / `<unregister>`
- **测试模式**: `setDryRun(true)` 打印日志不执行动作
- **自动清理**: 模块退出时自动清理订阅

## 构建库供其他项目使用

```bash
# 构建 Release 版本
cmake -B build -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# 安装
cmake --install build --config Release
```

使用时：
```cmake
add_library(msm_core STATIC IMPORTED)
set_target_properties(msm_core PROPERTIES
    IMPORTED_LOCATION "path/to/msm_core.lib"
)
target_include_directories(your_project PRIVATE "path/to/module_core/include")
target_link_libraries(your_project msm_core)
```

## 依赖

- C++17
- 无外部依赖 (内建 Json, MessageQueue)

## 许可证

MIT