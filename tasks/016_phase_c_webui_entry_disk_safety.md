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
- [ ] T3 方案：webui config 入口（含交互/门控/失败处理）
- [x] T4 方案：磁盘空间安全停止序列（2026-08-14 用户确认先做 T4）
- [ ] T5 实现 webui config 入口
- [x] T6 实现磁盘安全（P0-B）
- [x] T7 编译验证（msm_cli_simsrc/msm_cli_hwdaq/msm_webui/msm_config_editor 全过）
- [x] T8 集成验证 + 提交

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

## 设计决议（T4 磁盘安全，2026-08-14）
- 范围：仅 `module_storage` 内部增强，不改跨模块接口/配置格式
- 注入点：`onIdle()` 主写入循环每批前 `statvfs` 检查 `savePath_` 剩余空间
- 阈值：内部常量 `kMinFreeMB`（默认 1024 MB），不足 → 安全停止
- 停止序列：置 `diskEmergency_` → `MessageBus` 发 `Stop >> -@ DAQModule` 停采 →
  丢弃 ring 剩余（磁盘已满无法写）→ `fflush`+`fsync`+`fclose`+rename 收尾 → `recording_=false`
- `onStart` 同样检查磁盘，不足则不开启 recording（只丢弃）
- `rotateFile`/`closeFiles`：`fclose` 前加 `fflush`+`fsync(fileno)`，防掉电 rename 成不完整文件
- `onStatus`/变量表上报 `disk_free_mb`、`disk_emergency`

## 日志
- 2026-08-14 创建 task，进入阶段 C
- 2026-08-14 T1/T2 调研完成；T4 方案确认，先实现磁盘安全（P0-B）