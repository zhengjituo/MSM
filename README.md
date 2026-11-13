# MSM

大模型写单个函数已经胜任，进入系统级代码就开始失准：上下文装不下整条调用链，隐式耦合让影响面无法枚举，正确性只能靠整机验证。

MSM 的取向很明确：**以消息作为唯一的跨模块接口**，把系统改造成高内聚、低耦合、契约显式的形态。AI 每次的工作集收敛到一个模块加一份消息契约。

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)]() [![EPICS](https://img.shields.io/badge/EPICS-7.0-green)]() [![License](https://img.shields.io/badge/License-MIT-green)]()

![MSM 监控界面](docs/images/webui.png)

<sub>监控界面：帧计数、特征表、四通道波形实时刷新；图为仿真源数据。</sub>

## 三个失效模式，对应三处结构设计

### 一、上下文装不下调用链

模块自治。每个模块一个线程、一个消息队列、一个状态机，不引用兄弟模块的头文件，编译期即完成隔离。改一个模块，工作集就是它自己那份源码。

### 二、改一处崩一片，影响面不可枚举

跨模块只有 Message 一种通信原语，没有共享全局状态。影响面收敛为契约面：33 条消息命令，可枚举、可评审、可静态检查。

### 三、正确性只能靠整机验证

模块可独立实例化，以消息驱动做进程级测试。仿真源可直接替换采集硬件，测试不依赖整机，也不必等待仪器排期。

## 目录结构

```
module_core/       框架核心：MessageBus、Module 基类、VarTable、接口层
module_config/     配置模块，负责 pending → runtime 交接
module_daq/        采集模块，PXIe 1073 + 3010 硬件控制
module_process/    帧解析与特征提取（VPP / X / Y / 光强 / 波形）
module_storage/    批量落盘与文件轮转
module_simsource/  仿真数据源，无硬件时替代采集卡
main_cli/          CLI 入口，内建 IPC server
main_webui/        浏览器监控入口（IPC ↔ HTTP + SSE + ECharts）
main_epics/        EPICS CA 桥与 PyDM 控制界面
config/            共享配置，pv_map.json 是 PV 的唯一真源
sdk_include/       硬件 SDK 头文件
docs/  tasks/      设计文档与可追溯的任务流水
```

另有 `module_calculator`、`module_datachannel`、`main_config_editor`、`tools/` 等辅助模块，按需使用。

## 用法

```bash
cmake -B build && cmake --build build -j

./main_cli/build/msm_cli_simsrc   # 仿真采集，4 通道，无需硬件
./main_cli/build/msm_cli_hwdaq    # 真实硬件，1 GSa/s
./main_webui/build/msm_webui      # 浏览器端波形与状态监控

./main_epics/run_epics.sh         # 起 IOC 与 CA 桥
./main_epics/run_pydm.sh          # PyDM 控制界面
```

CLI 命令：`start` / `stop` / `enablesave` / `disablesave` / `status` / `quit`。
配置改动写入 `config/app_pending_config.json`，每次 `start` 时后端完成 pending → runtime 交接并重配硬件，不需要 reload。

## 示例：新增一个模块

三件事：继承 `Module`，实现 `handleState()`，用 `declareVar()` 声明变量。声明即对外，接口层零配置。

<details>
<summary>展开看代码</summary>

```cpp
class MyModule : public msm::Module {
public:
    MyModule() : Module("MyModule") {
        declareVar("counter", counter_);      // 声明即对外，接口层自动发现
    }
protected:
    void handleState(const msm::Message& msg) override {
        if (msg.cmd == "Configure")
            periodMs_ = msg.data.getInt("periodMs");
        else if (msg.cmd == "Start")
            running_ = true;
        else if (msg.cmd == "Stop")
            running_ = false;
    }
    void onIdle() override {
        if (running_) counter_.fetch_add(1);
    }
private:
    std::atomic<int>  counter_{0};
    std::atomic<bool> running_{false};
    int periodMs_ = 100;
};
```

装配与调用：

```cpp
auto& bus = msm::MessageBus::instance();

MyModule mod;
bus.registerModule(&mod);
mod.start();                                  // 独立线程开始轮转

msm::Message cfg("Configure");
cfg.setTarget("MyModule");
cfg.data.set("periodMs", 20);
bus.send(cfg);                                // 异步；需要返回值用 sendAndWait
```

</details>

`Status` 不需要自己实现：基类按 VarTable 自动生成，`caget` / REST / IPC 拿到的是同一份数据。上层入口不同，落到模块里的都是同一个 `Message`。

## 设计文档

- `docs/arch/design.md` 框架设计：消息模型、MessageBus、接口层
- `tasks/` 29 份决策与实测记录
- `docs/arch/MSMvsEPICS.md` 与 EPICS、Bluesky 的架构对比

---

消息语法沿用 NI CSM（`-@`、`->`、`->|`、`<broadcast>`、`<register>`），LabVIEW 技术栈零迁移成本。当前为单机部署，多机调度与硬实时指标尚未覆盖。
