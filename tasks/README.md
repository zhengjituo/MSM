# Task 索引

任务文件统一为 `NNN_<desc>.md`：起始 commit、TODO、执行日志、done、结束 commit。工作流见 `AGENTS.md`。

| 编号 | 任务 | 日期 |
|:--|:--|:--|
| 025 | 扩展 MessageBus 支持外部消息注入 | 2026-04-22 |
| 026 | Module 独立 CLI 终端 | 2026-04-28 |
| 027 | ZMQServer + RESTServer 实现 | 2026-06-04 |
| 028 | WebGUI Restore | 2026-07-09 |
| 003 | 目录重构 + Workflow 落实 | 2026-07-21 |
| 004 | calculator/datachannel → module_core/tests/，module_sim → module_simsource | 2026-07-21 |
| 005 | 删除旧版框架 `msm/` | 2026-07-21 |
| 006 | V1: Storage discard mode（读取丢弃，不写盘） | 2026-07-21 |
| 007 | V1 CLI stable + IPC 交互 / stop drain + ready 信号 | 2026-07-22 |
| 008 | 修订 V2/V3 路线图 + main_simsrc 对齐 hwdaq | 2026-07-22 |
| 004 | enablesave/disablesave + saveRing 对齐修复 | 2026-07-22 |
| 008 | ConfigModule：可复用的配置管理模块 | 2026-07-24 |
| 009 | WebUI 后端进程管理 + C++ frame_parser 库 | 2026-07-27 |
| 010 | 波形原始数据提取 + ECharts 前端波形显示 | 2026-07-29 |
| 011 | EffectiveDataExtraction 参数格式修正 (hex→decimal) + WorkMode | 2026-07-29 |
| 012 | webui 后台进程优雅退出，xdma 资源释放 | 2026-08-12 |
| 013 | webui 优雅退出：svr.stop() + SIGHUP | 2026-08-12 |
| 014 | 独立 config 编辑器（module_config + msm_config_editor） | 2026-08-13 |
| 015 | config 编辑器合并进主程序（阶段 B） | 2026-08-13 |
| 016 | 阶段 C：webui config 入口 + 磁盘空间安全（P0-B） | 2026-08-14 |
| 017 | Config 单链式生效：pending → runtime，移除 reload | 2026-08-24 |
| 018 | 多卡采集支持（≤4×1073，严格 slot 匹配） | 2026-10-21 |
| 019 | 双卡深挖：丢帧 / 处理效率 / bunch 相位 / 波形 tab | 2026-10-26 |
| 020 | PV 推送限频（2–10Hz）+ 投影单向化 | 2026-11 |
| 021 | PyDM 控制界面，操作逻辑对齐 webui | 2026-11 |
| 022 | 合并 EPICS 分支（main_epics/PyDM）与多卡分支 | 2026-11-16 |
| 023 | PyDM 多卡 tab + 波形/退出三个隐性 bug | 2026-11-17 |
| 024 | 丢帧治理：overflow 暴露 + ring 可配 + 消费瓶颈计时 | 2026-11-18 |

> 025–028 为日期制时代（原 `2026MMDD_*.md`）早期任务的补编号：编号是序号，不表示时间先后，时序以日期列为准。004 / 008 各有两条，为历史编号撞车。

## 遗留文档（已归档至 `docs/archive/`）

| 文档 | 说明 |
|:--|:--|
| `20260616_handoff.md` | 工作移交文档（2026-06-16） |
| `20260618_msm_core_upgrade_plan.md` | MSM Core 升级计划 |
| `20260618_msm_daq_epics.md` | EPICS 集成交接文档 |
| `20260618_msm_daq_handoff.md` | MSM_DAQ → module_core 升级交接文档 |
| `requirements.md` | 早期 SHINE_CSM 需求列表 |
| `tasks_history_shine_csm.md` | SHINE_CSM 时代任务流水（原 tasks/README.md） |
