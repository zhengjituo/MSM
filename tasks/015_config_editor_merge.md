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

## TODO
- [x] T1 调研主程序 app_runtime_config.json / app_initial_config.json 加载机制
- [x] T2 主程序 reload 改为从 runtime 加载（main_hwdaq + main_simsrc 各 2 处）
- [x] T3 编辑器默认操作 app_runtime_config.json
- [x] T4 Tab2 勾选「保存到启动配置」→ 写 app_initial_config.json
- [x] T5 严格保真写回验证（主程序 loadFromJson 读回 OK）
- [x] T6 集成验证 + 提交 + 完结 task
- [ ] T7（后续）webui 入口按钮 + 磁盘空间安全（P0-B）

## 合并日志（2026-08-14）
- T2：main_hwdaq.cpp / main_simsrc.cpp reload 两处改为 `loadFromJson(g_runtimeConfigPath)`，
  去掉了 reload 时 saveToJson(runtime)（避免覆盖编辑器严格保真写回）；simsrc 保留 slot=0 特殊逻辑
- T3：编辑器默认操作文件 runtime；meta 暴露 runtimeFile/initFile
- T4：save-app 支持 saveToInit → `applyAppEditToFile()`（读目标文件→合并 app_config→原子写回，含类型保护）
- T5：验证主程序自写解析器能读回编辑器写出的 runtime（cards=2, ch1=4, runId=888 正确）
- 验证：编辑器回归 ALL PASS；新语义（runtime only / runtime+init / 类型拒绝）全部通过
- 待办：webui 入口（config 按钮）+ P0-B 磁盘安全（阶段 C）

## 合并设计（2026-08-13 已确认）
- 功能分离：`config`=配置加载（reload 从 runtime 加载），`start`=采集启停
- 启动仍从 initial 加载生成 runtime（不变）
- reload 2 处改为 `loadFromJson(g_runtimeConfigPath)`（而非 g_initConfigPath）
- 编辑器主操作文件 = app_runtime_config.json、严格保真写回（ordered_json保留字段/类型）
- Tab2 勾选「保存到启动配置」→ 额外原子写 app_initial_config.json（启动加载文件）

## 日志
- 2026-08-13 阶段 A 完结（commit 7a0a723），开始阶段 B 合并

## 结束 commit
```
cd44050  merge: config editor operates app_runtime_config.json; reload loads from runtime; optional save to initial config (saveToInit)
```
T1~T6 完成。T7 阶段 C（webui 入口按钮 + 磁盘空间安全 P0-B）待后续。
