# MSM_DAQ 项目顶层入口

## 目录结构

```
MSM_DAQ/
├── AGENTS.md               # 顶层入口：目录结构 + Agent workflow 约束
├── .gitignore
├── CMakeLists.txt           # [可选] 根一键构建（add_subdirectory 各模块）
│
├── docs/                    # 设计文档（详细索引见 docs/README.md）
│   ├── README.md            # 目录索引 + 阅读优先级
│   ├── arch/                # [P1] 框架架构（必读）
│   ├── modules/             # [P2] 各模块设计
│   ├── data_formats/        # [P3] 数据结构
│   └── archive/             # [P4] 历史
│
├── tasks/                   # 可追溯的 task 流水号 .md
│   ├── 003_reorg_workflow.md
│   └── <NNN>_<desc>.md      # 含：TODO、起始 commit、执行日志、done、结束 commit
│
├── config/                  # 共享配置
├── extern/                  # 共享第三方依赖
├── sdk_include/             # 硬件 SDK 头文件
│
├── module_core/             # 框架核心（MessageBus、Module 基类等）
├── module_daq/              # 采集模块（1073 + 3010）
├── module_process/          # 处理模块（轻量 peek 版）
├── module_process_full/     # 处理模块（全帧提取版，含跨通道对齐检查）
├── module_storage/          # 存盘模块
├── module_simsource/          # 虚拟数据源（仿真模式）
│
├── main_cli/                # CLI + IPC 用户接口
│   ├── main_simsrc.cpp      → msm_cli_simsrc（仿真）
│   ├── main_hwdaq.cpp       → msm_cli_hwdaq（硬件）
│   ├── msm_ipc_shell.cpp    → msm_ipc_shell（独立 IPC 终端）
│   └── CMakeLists.txt
├── main_webui/              # WebUI（IPC-to-HTTP 桥接）
│   ├── main_hwdaq.cpp       → msm_webui
│   └── web/
├── module_core/tests/
│   ├── calculator/          # 框架 demo（AddModule/MultiplyModule, IPC 通信示例）
│   └── datachannel/         # 跨进程 DataChannel 测试
│
└── [旧残留]                  # 不纳入新结构，不删除
    ├── MSMvsEPICS*.md
    └── ...
```

## Workflow（Agent 约束）

**总原则：每次修改可追溯、可回退。**

### 步骤
1. **写 task 文件** — `tasks/<NNN>_<desc>.md`，记录起始 commit、TODO 清单
2. **确认已备份** — 检查 `git status` 干净，否则先 commit
3. **做修改** — 改代码、搬文件等
4. **编译验证** — 受影响的构建目标必须通过
5. **git commit** — 简短描述做了什么
6. **完结 task** — 在 task 文件中填入结束 commit，标记 done

### 子 Agent 协作
- 如需分配给子 Agent 执行，在 prompt 中 **指明需预读的参考文件路径**（如 `docs/arch/msm_daq的定义.md`、`module_daq/module_daq.cpp`）
- 不要让它无差别浏览所有文件，避免 token 浪费和上下文偏移

### Git 提交准则
- 只提交源代码（`.cpp`/`.h`/`.py`/`CMakeLists.txt`/`config/*.json`/`docs/*.md`/`AGENTS.md`）
- 不提交构建产物（`build/`）、编译出的二进制（`main_ref_test`、`trigger3010`）、运行时文件（`runtime_config.json`、`.msm_session`）
- `.gitignore` 已排除上述内容

### 当前版本状态
- **main_cli** — 稳定。CLI + IPC 交互，支持 Storage 录盘
  - 二进制：`msm_cli_hwdaq`（硬件）、`msm_cli_simsrc`（仿真）、`msm_ipc_shell`（IPC 终端）
  - IPC 路径：`/tmp/msm_daq.sock`
  - 命令：`start` / `stop` / `enablesave` / `disablesave` / `status` / `reload` / `quit`
- **main_webui** — 开发中。IPC-to-HTTP 桥接 + Web GUI

### 目录设计规则
- 模块扁平化：`module_xxx/`，共享依赖在顶层
- `docs/` 下每模块有同名 `.md` 设计说明（功能简介、输入、输出、状态帧说明）
- 顶层的 `main` 伪装成 `module_main_v*/`，与子模块并列访问
- 每个 `module_main_v*/` 可有独立 `CMakeLists.txt` + `build/`，也可通过根 CMakeLists.txt 一键构建
- 旧残留保留不纳入新结构
