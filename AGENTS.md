# MSM_DAQ 项目顶层入口

## 目录结构

```
MSM_DAQ/
├── AGENTS.md               # 顶层入口：目录结构 + Agent workflow 约束
├── .gitignore
├── CMakeLists.txt           # [可选] 根一键构建（add_subdirectory 各模块）
│
├── docs/                    # 架构设计文档
│   └── *.md                 # 每模块同名 .md 设计说明
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
├── module_sim/              # 虚拟数据源
│
├── module_main_v1/          # main: 仿真模式（msm_daq_sim）
├── module_main_v2/          # main: 硬件全功能 display（msm_daq_display）
├── module_main_v3/          # main: 硬件采集存盘（msm_daq_storage）
│
└── [旧残留]                  # 不纳入新结构，不删除
    ├── module_calculator/
    ├── module_datachannel/
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

### Git 提交准则
- 只提交源代码（`.cpp`/`.h`/`.py`/`CMakeLists.txt`/`config/*.json`/`docs/*.md`/`AGENTS.md`）
- 不提交构建产物（`build/`）、编译出的二进制（`main_ref_test`、`trigger3010`）、运行时文件（`runtime_config.json`、`.msm_session`）
- `.gitignore` 已排除上述内容

### 目录设计规则
- 模块扁平化：`module_xxx/`，共享依赖在顶层
- `docs/` 下每模块有同名 `.md` 设计说明（功能简介、输入、输出、状态帧说明）
- 顶层的 `main` 伪装成 `module_main_v*/`，与子模块并列访问
- 每个 `module_main_v*/` 可有独立 `CMakeLists.txt` + `build/`，也可通过根 CMakeLists.txt 一键构建
- 旧残留保留不纳入新结构
