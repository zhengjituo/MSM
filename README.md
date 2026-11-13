# MSM

**面向 AI 编码的模块化数据采集框架。** 以消息作为唯一的跨模块接口，使系统级改动在三个维度上可控：上下文、影响面、验证成本。

## 论证链

**① 上下文装不下调用链**
→ 模块即上下文单元：独立线程、消息队列、状态机，编译期与兄弟模块隔离
→ **可审查**

**② 隐式耦合使影响面不可枚举**
→ 消息是唯一的跨模块接口，无共享全局状态
→ **可定位**

**③ 正确性只能依靠整机验证**
→ 模块可独立实例化、以消息驱动，硬件可被仿真源替换
→ **可测试**

三条约束收敛到同一结论：任一改动的边界，等于一个模块加一份消息契约。

![MSM 监控界面](docs/images/webui.png)

<sub>监控界面：帧计数、特征表、四通道波形实时刷新；图为仿真源数据。</sub>

## 约束的落地形态

### 模块自治

每个模块持有独立线程、消息队列与状态机，头文件不引用兄弟模块。模块边界与编译边界重合，改动的工作集即该模块源码，评审单位即该模块。

### 消息即契约

跨模块只存在 `Message` 一种通信原语：33 条命令、28 个对外变量声明。契约面可枚举、可评审、可静态检查。IPC / REST / EPICS 等接口层由变量声明自动导出，不构成额外契约。

### 进程级可验证

模块可脱离系统独立实例化，以消息序列驱动状态迁移；`module_simsource` 可替换采集硬件，验证不依赖整机与仪器排期。

## 目录结构

```
module_core/       框架核心：MessageBus、Module 基类、VarTable、接口层
module_config/     配置模块，负责 pending → runtime 交接
module_daq/        采集模块，PXIe 1073 + 3010 硬件控制
module_process/    帧解析与特征提取（Vpp / X / Y / 光强 / 波形）
module_storage/    批量落盘与文件轮转
module_simsource/  仿真数据源，无硬件时替换采集卡
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

./main_cli/build/msm_cli_simsrc   # 仿真采集，4 通道，不依赖硬件
./main_cli/build/msm_cli_hwdaq    # 真实硬件，1 GSa/s
./main_webui/build/msm_webui      # 浏览器端波形与状态监控

./main_epics/run_epics.sh         # 启动 IOC 与 CA 桥
./main_epics/run_pydm.sh          # 启动 PyDM 控制界面
```

CLI 命令：`start` / `stop` / `enablesave` / `disablesave` / `status` / `quit`。
配置变更写入 `config/app_pending_config.json`，于下一次 `start` 时由后端完成 pending → runtime 交接并重配硬件，无需 reload。

## 示例：新增一个模块

继承 `Module`、实现 `handleState()`、以 `declareVar()` 声明对外变量。声明即导出，接口层零配置。

<details>
<summary>展开看代码</summary>

```cpp
class MyModule : public msm::Module {
public:
    MyModule() : Module("MyModule") {
        declareVar("counter", counter_);      // 声明即导出
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

`Status` 无需自行实现：基类按 VarTable 生成，`caget`、REST、IPC 取得同一份数据。入口不同，落到模块内的均为同一个 `Message`。

## 设计文档

- `docs/arch/design.md` 框架设计：消息模型、MessageBus、接口层
- `tasks/` 29 份决策与实测记录
- `docs/arch/MSMvsEPICS.md` 与 EPICS、Bluesky 的架构对比

---

消息语法沿用 NI CSM（`-@`、`->`、`->|`、`<broadcast>`、`<register>`），对 LabVIEW 技术栈无迁移成本。EPICS 桥、config CLI、PyDM 前端均为后加能力，未改动既有消息契约，以新增订阅方接入。当前为单机部署，多机调度与硬实时指标未覆盖。
