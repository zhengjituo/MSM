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
- [ ] T1 调研 main_webui 结构（按钮位置、进程拉起机制、状态指标）
- [ ] T2 调研 module_storage 写入循环与 flush 时序，确定磁盘检查注入点
- [ ] T3 方案：webui config 入口（含交互/门控/失败处理）
- [ ] T4 方案：磁盘空间安全停止序列
- [ ] T5 实现 webui config 入口
- [ ] T6 实现磁盘安全（P0-B）
- [ ] T7 编译验证
- [ ] T8 集成验证 + 提交 + 完结 task

## 日志
- 2026-08-14 创建 task，进入阶段 C