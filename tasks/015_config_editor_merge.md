# Task 015 — config 编辑器合并进主程序（阶段 B）

## 备份 git 号（恢复点）
```
7a0a723  fix: preserve app_config value types on save - frontend collects by JSON type, backend rejects type mismatch (400)
```
> 阶段 B 开发中如需回退，`git reset --hard 7a0a723` 恢复。备份前工作区仅含
> untracked `config/app_initial_config_backup.json`（测试中介，不入库）。

## 目标（阶段 B 合并）
- 将阶段 A 的独立 config 编辑器并入主程序使用流程：
  - **主操作文件**：`config/app_runtime_config.json`（主程序运行时配置）
  - **第二 tab（软件参数）**：勾选「保存到启动配置」→ 额外写入
    `config/app_initial_config.json`（启动时加载文件，会覆盖下次启动加载的初始配置）
- 交集点（阶段 A 设计）：
  1. config 按钮 + 运行状态监视的启动调用（webui 入口）
  2. 配置文件 json 的读写交互
- 阶段 A 已完成：`ConfigEditorModule` + `msm_config_editor` REST + webUI + 类型保护

## 设计要点（沿用阶段 A）
- `ConfigEditorModule : msm::Module` 挂独立 MessageBus；HTTP handler 用 sendAndWait
- 内部 ~2s 探测 DAQ IPC（`MSG Status >> -@ DAQModule`）→ acquiring_；采集中保存 409
- 原子写 tmp.<pid> + fsync + rename；版本迁移 .bak + 增量补默认
- JSON 兼容：ordered_json 往返保键序/未知字段/值类型；类型保护已实现

## TODO（待调研后细化）
- [ ] T1 调研主程序 app_runtime_config.json / app_initial_config.json 加载机制
- [ ] T2 确认编辑器默认操作 app_runtime_config.json
- [ ] T3 Tab2 勾选「保存到启动配置」→ 写 app_initial_config.json
- [ ] T4 webui 入口按钮（config 按钮 + 运行状态监视）
- [ ] T5 磁盘空间安全（P0-B，module_storage）
- [ ] T6 集成验证 + 提交 + 完结 task

## 日志
- 2026-08-13 阶段 A 完结（commit 7a0a723），开始阶段 B 合并
