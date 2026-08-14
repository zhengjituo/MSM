# Task 016 — 阶段 C：webui config 入口 + 磁盘空间安全（P0-B）

## 备份 git 号（恢复点）
```
f256631  task 015: close merge Phase B (reload from runtime + saveToInit)
```
> 阶段 C 开发中如需回退，`git reset --hard f256631`。备份前工作区仅含
> untracked `config/app_initial_config_backup.json`（测试中介，不入库）。

## 目标（阶段 C）
1. **webui config 入口**：注入 config 按钮（+ 运行状态监视启动调用），
   拉起 `msm_config_editor`（config 编辑器主操作 app_runtime_config.json）
2. **磁盘空间安全停止（P0-B）**：`module_storage` 磁盘不足 → 安全停止序列

## 设计要点
- config 按钮：检测 DAQ 未采集（非采集状态）才可进入；启动编辑器进程并打开页面
- 运行状态监视：复用 DAQ 门控探测（`MSG Status >> -@ DAQModule` 解析 running）
- 磁盘安全（P0-B）：`module_storage` 写入路径周期检查剩余空间，< 阈值 → 停采→等 flush→
  关闭保存→优雅退出；原因上报

## TODO
- [x] T1 调研 main_webui 结构（按钮位置、进程拉起机制、状态指标）
- [x] T2 调研 module_storage 写入循环与 flush 时序，确定磁盘检查注入点
- [x] T3 方案：webui config 入口（含交互/门控/失败处理，2026-08-14 用户确认）
- [x] T4 方案：磁盘空间安全停止序列（2026-08-14 用户确认先做 T4）
- [x] T5 实现 webui config 入口
- [x] T6 实现磁盘安全（P0-B）
- [x] T7 编译验证（msm_cli_simsrc/msm_cli_hwdaq/msm_webui/msm_config_editor 全过）
- [x] T8 集成验证 + 提交

## 集成验证（T3 webui config 入口）
- `/api/run sim` 后端 ready ✓
- `/api/config` 拉起 `msm_config_editor`，8081 `/api/meta` 200 ✓
- `editor_alive` 状态经 poll 快照正确上报（launch 后 True，close 后 False）✓
- `/api/config/close` 优雅关闭（`[config-editor] shutdown` 日志确认）✓
- webui 退出自动回收编辑器进程 ✓
- 前端页面含 Config 按钮/门控/editor_alive marker 渲染正常 ✓
- 全 target 构建通过 ✓

## 集成验证（T4 磁盘安全）
- 紧急路径（临时调阈值 4000000MB 模拟磁盘不足）：
  - `[Storage] DISK EMERGENCY: free 3546415 MB < min, stopping acquisition` ✓
  - `[Storage] Started, save DISABLED (disk below threshold)` ✓（onStart 检查生效）
  - 随后 `[DAQModule] Stopped`（triggerDiskEmergency 发的 Stop 停采）✓
- 正常路径（恢复阈值 1024MB）：
  - `Started, recording`，4 通道各 30000 帧 = 34,560,000 字节完整落盘 ✓
  - tmp → rename 成 final `.dat` 成功（fsync 序列无副作用）✓
  - 磁盘充足无误触发 ✓
- 验证后已清理 `/mnt/localssd/fel1/HSS/PD/20260814/` 测试数据

## 日志
- 2026-08-14 创建 task，进入阶段 C
- 2026-08-14 T1/T2 调研完成；T4 方案确认，先实现磁盘安全（P0-B）
- 2026-08-14 T4 实现完成：statvfs 检查（onStart + onIdle 主循环）+ 紧急停止序列 +
  fsync 持久化 + status/变量表上报；编译+集成验证通过
- 2026-08-14 T3/T5 实现完成：webui Config 入口（后端拉起/关闭 msm_config_editor +
  2 端点 + editor 状态上报；前端 Config/Close 按钮 + 门控）。阶段 C 全部完成

## 设计决议（T4 磁盘安全，2026-08-14）
- 范围：仅 `module_storage` 内部增强，不改跨模块接口/配置格式
- 注入点：`onIdle()` 主写入循环每批前 `statvfs` 检查 `savePath_` 剩余空间
- 阈值：内部常量 `kMinFreeMB`（默认 1024 MB），不足 → 安全停止
- 停止序列：置 `diskEmergency_` → `MessageBus` 发 `Stop >> -@ DAQModule` 停采 →
  丢弃 ring 剩余（磁盘已满无法写）→ `fflush`+`fsync`+`fclose`+rename 收尾 → `recording_=false`
- `onStart` 同样检查磁盘，不足则不开启 recording（只丢弃）
- `rotateFile`/`closeFiles`：`fclose` 前加 `fflush`+`fsync(fileno)`，防掉电 rename 成不完整文件
- `onStatus`/变量表上报 `disk_free_mb`、`disk_emergency`

## 设计决议（T3 webui config 入口，2026-08-14 用户确认）
- 范围：仅 `main_webui`（后端 + 前端 HTML），不改跨模块 Message/配置格式
- 后端：复用 `launchBackend` 的 fork+`findCliPath` 模式拉起 `msm_config_editor`（监听 8081）
  - 新端点 `POST /api/config`：启动编辑器进程（已跑则返回状态）；`POST /api/config/close`：向 8081 发 `/api/shutdown`
  - 编辑器进程 pid/状态独立于后端（`pidEditor_`），随 webui 退出时回收
- 门控双保险：
  1. 前端按钮 `disabled = d.running || !backendRunning`
  2. 编辑器侧 lock（`config_editor_module` 经 DAQ socket running → acquiring_，采集中 save 返回 409）
- 前端：toolbar 加 `⚙ Config` 按钮 + 状态指示；点击 → POST /api/config → `window.open('http://localhost:8081')`
- 失败处理：fork 失败/不可达 → toast 报错

## 阶段 C.5 — config 编辑器合并进 webui（2026-08-14）
- 动机：close 按钮竞态/PID 复用/SIGKILL 兜底、editor_alive 状态错乱、acquiring 门控失效
  （probe 字符串匹配 bug：`"running": "true"` 带引号，`"running":true` 永不匹配）
- 方案：ConfigEditorModule 以 MessageBus 消息并入 webui 进程；删进程管理+门控；
  reload 剥离 Stop，仅空闲可用
- 决策：保留 main_config_editor 源码，停构建；运行中保存只写文件不自动生效

## 日志
- 2026-08-14 创建 task，进入阶段 C
- 2026-08-14 T1/T2 调研完成；T4 方案确认，先实现磁盘安全（P0-B）
- 2026-08-14 阶段 C.5 config 编辑器合并进 webui 完成：
  - 932be8b 剥离 acquiring 门控与 probe 线程
  - b1b5575 嵌入 ConfigEditorModule 状态机（链接 module_config，停构建 main_config_editor）
  - e55305f 删除编辑器子进程管理全链路 + editor_alive
  - 6f42c6e 新增 /api/config/{meta,config,save,save-app,reload} 端点（MessageBus sendAndWait）
  - f71c9a6 reload 剥离 Stop（CLI + IPC 两分支），仅空闲门控
  - e6eb18f 前端内嵌 Config 面板（cfgLoad/cfgRenderParams/doCfgSave/doCfgReload），reload 按钮采集中禁用
  - 集成验证：launch sim → running；reload 空闲 `{"OK":"reload sent"}`；start→running:true 保持，
    reload 不破坏采集；stop→false；meta/config/save/save-app 端点均 ok；测试进程已清理