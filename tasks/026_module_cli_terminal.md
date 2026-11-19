# Task 026 — Module 独立 CLI 终端

> 2026-04-28（补编号，原文件名 `20260428_01_module_cli_terminal.md`）

## 状态
- [x] 已完成

---

## todo: 计划

### 需求

MSM 框架当前缺少人类交互式调试能力：
- injectMessage 适合自动化测试
- 无法手动单步调试
- 无法实时查看变量
- 类似 LabVIEW VI 的独立前面板无法实现

### 目标

为每个 Module 增加独立 CLI 终端：
1. 每个 Module 拥有独立 CLI 终端线程（前端）
2. 终端可独立打开/关闭，关闭后后台继续运行
3. 通过终端 GET/SET 读写 Module 变量
4. 主终端可管理所有 Module 终端

### 生命周期命令

| 命令 | 说明 |
|------|------|
| **Load** | 加载配置到内存 |
| **Run** | 启动 Module 线程，进入 Initial → Idle |
| **Exit** | 安全退出消息（可重新 Load） |
| **Release** | 释放内存（几乎不用） |

### 终端命令

| 命令 | 说明 |
|------|------|
| **get \<name\>** | 读取变量 |
| **set \<name\> \<value\>** | 写入变量 |
| **list** | 列出所有变量 |
| **status** | 显示 Module 状态 |
| **quit** | 关闭终端（后台继续） |

### 设计

```
┌─────────────────────────────────────────────────────┐
│                    主进程                              │
├─────────────────────────────────────────────────────┤
│  主终端 (管理所有 Module)                              │
│  > list                                             │
│  > open <module_name>                               │
│  > quit                                             │
├─────────────────────────────────────────────────────┤
│  Module_A (线程)                                      │
│  ┌───────────────────┐  ┌─────────────────────┐    │
│  │ 后台状态机线程    │  │ 独立 CLI 终端线程   │    │
│  │ (runLoop)         │  │ (CLIInterface)      │    │
│  └───────────────────┘  └─────────────────────┘    │
└─────────────────────────────────────────────────────┘
```

### 子任务拆解

| ID | 子任务 | 说明 |
|----|--------|------|
| T1 | CLIInterface 基类 | 独立线程运行 CLI，支持 get/set/list/status/quit |
| T2 | Module 集成 | 每个 Module 内置 CLIInterface，open/close 接口 |
| T3 | 变量访问接口 | getVariables() / setVariableValue() 子类重写 |
| T4 | 主终端管理器 | 列出/打开/关闭各 Module 终端 |
| T5 | 测试验证 | 用 AddModule 测试完整流程 |

### 修改范围

- `module_core/include/Module.h` - 添加 CLI 接口
- `module_core/src/Module.cpp` - 实现 CLI 接口
- `module_core/include/CLIInterface.h` - (新增)
- `module_core/src/CLIInterface.cpp` - (新增)
- `test/` - 添加 CLI 测试

---

## done: 结果

### T1 CLIInterface 基类 ✅
- 创建 `module_core/include/CLIInterface.h`
- 创建 `module_core/src/CLIInterface.cpp`
- 独立 CLI 线程，支持命令：get/set/list/status/help/quit

### T2 Module 集成 ✅
- 更新 `module_core/include/Module.h` - 添加 CLI 接口
- 更新 `module_core/src/Module.cpp` - 实现 CLI 接口
- openTerminal()/closeTerminal()/isTerminalOpen()
- 变量访问接口：getVariables()/getVariableValue()/setVariableValue()

### 编译验证 ✅
- 编译成功（有 C4819 Unicode 警告）
- 测试通过

### T4 主终端管理器 ✅
- 创建 `module_core/include/TerminalManager.h`
- 创建 `module_core/src/TerminalManager.cpp`
- list/open/close/status 命令
- Module 构造函数自动注册

### T5 CLI 测试验证 ✅
- 创建 `module_core/test/test_cli.cpp`
- 测试变量 get/set/list
- 测试 terminal open/close
- 测试 invalid variable 拒绝

---

## note: 附注

- 参考 NI VI 模式：前面板关闭，后台继续运行
- 每个 MSM 独立终端 = 微服务架构
- 轻量替代方案：先实现本地 CLI，暂不考虑远程 SSH