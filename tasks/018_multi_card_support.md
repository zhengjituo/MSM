# Task 018 — 多卡采集支持（最多 4×1073，严格 slot 匹配）

## 备份 git 号（恢复点）
```
4fee053  docs(plan): config pending-chain implementation plan (task 017)
```
> 回退：`git reset --hard 4fee053`。分支：`feat/multi-card`（worktree）。

## 目标
1. 最多 4 张 1073（16 通道），DAQ/Process/Storage/WebUI 全链路支持
2. 打开数量 = 配置 slot ∩ 机箱扫描 slot（严格匹配，不兜底）
3. 缺卡/槽位不符/超限 → 报错 + 日志，Init 失败不采集，进程存活
4. 3010 配置有则必须存在，缺失同样报错
5. ring 大小 `app_config.RingSizeMB` 可配（默认 256MB）
6. 通道编号 = 配置 `Card#N` 顺序 × 卡内通道顺序
7. 配置 UI 仿原 Qt：当前板卡下拉 + 3 按钮（通道 / 当前板卡 / 机箱全部）

设计：`docs/superpowers/specs/2026-08-24-multi-card-support-design.md`
计划：`docs/superpowers/plans/2026-08-24-multi-card-support.md`

## TODO
- [ ] T1 module_daq：严格校验 + 交集 open + 集合变更重开 + Init 错误响应
- [ ] T2 main 链：Init sendAndWait + last_start_error
- [ ] T3 config：RingSizeMB 解析/序列化 + mains 使用
- [ ] T4 Process/Storage 文案去 4 通道硬编码
- [ ] T5 WebUI 波形动态网格（4/8/12/16）
- [ ] T6 配置面板 3 按钮 + 添加板卡 + 扫描 slot 提示
- [ ] T7 编译 + 仿真多卡验证 + 真机冒烟 + 文档收尾

## 验证方式
- 编译：msm_cli_hwdaq / msm_cli_simsrc / msm_ipc_shell / msm_webui
- 仿真：2 卡 8ch、4 卡 16ch 跑 SSE/特征/波形/存盘
- 错误路径：配置 slot 不存在 → 日志列出缺失与实际 slots，不采集
- 真机：单卡回归 + 双卡；单卡配置 → 双卡配置 Start 自动重开

## 日志
- 2026-08-24 设计确认（严格 slot / 缺卡报错 / RingSizeMB / 配置 UI 仿 Qt）；
  本 task 创建
