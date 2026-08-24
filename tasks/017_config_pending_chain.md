# Task 017 — Config 单链式生效：pending → runtime，移除 reload

## 备份 git 号（恢复点）
```
eac7c74  task 016 收尾（config-panel spec 标记已实施）
```
> 开发中如需回退：`git reset --hard eac7c74`。分支 `feat/config-pending-chain`
> （worktree `.worktrees/config-pending-chain`）。

## 目标
1. 废弃「运行中 reload 热生效」机制（2026-08-18 12:53 rotateFile 崩溃根因：
   运行中热改配置 + 字符串成员被破坏）
2. webui 只编辑 pending 配置；后端在 Start 时自主完成
   **pending→runtime 交接 + 重载配置 + 重配硬件 + 开始采集**
3. 彻底移除 reload：webui Reload 按钮/`/api/config/reload`、CLI reload 命令、IPC RELOAD 分支

设计文档：`docs/superpowers/specs/2026-08-19-config-pending-chain-design.md`

## 文件模型
```
app_initial_config.json   出厂模板（仅首启播种源）
 → app_pending_config.json   待使用配置（webui 编辑目标；首启不存在时从 initial 播种）
 → app_runtime_config.json   后端实际加载的运行配置（Start 交接时由 pending 原子覆盖）
```
原子写：tmp + fsync + rename。

## 最终语义（2026-08-19 用户确认）
每次 Start（前端仅在硬件未运行时可点）执行完整链：
```
ensureRuntimeConfig(pending→runtime) → configModule.loadFromJson(runtime)
→ Init(hwdaq) / Configure(simsrc) 重配硬件 → Start 三连
```
IPCServer START 分支只转发 `Message("Start")` 给 ControllerModule；
Controller handleState 调全局 `g_startFn`（CLI stdin start 走同一入口）。

## TODO
- [x] T1 设计 spec（f83e9b1，2026-08-19 用户确认最终语义）
- [x] T2 `config_handoff` 助手 ensureRuntimeConfig（8932181）
- [x] T3 main_hwdaq 接入交接 + 移除 reload/g_reload（8e5c856）
- [x] T4 main_simsrc 同上（4bdd9ef）
- [x] T5 webui 编辑目标改 pending + 首启播种 + 删 /api/config/reload（b388cdc）
- [x] T6 前端删 Reload 按钮/saveToInit，提示改「下次启动生效」（9fc45a2）
- [x] T7 config_editor 默认编辑 pending（f4d9d8b）
- [x] T8 gitignore pending（c04e665）；IPCServer START→Controller、删 RELOAD、
      两 main 加 g_startFn、loadFromJsonString cards 叠加修复（20baf33）
- [x] T9 端到端验证（见下）

## 实施记录（关键点）
- ModuleDAQ Init 天生支持热重配：hwReady_ 时跳过 initHW 重开、仅 updateConfig 推参
  （module_daq.cpp:76-79）；ModuleSim Configure 可重入，动态改 trigLength/frameSize
- loadFromJsonString 曾有 cards 叠加 bug（二次加载卡片翻倍），函数开头补
  `cards.clear(); slot = 0;` 修复
- httplib 默认 SO_REUSEPORT（httplib.h:1550）：多个 webui 实例可共存 8080 并分流请求，
  曾致「保存后第一次点 Config 不刷新」（旧进程响应）。排查发现 6 个残留实例，清理后正常。
  部署注意：换二进制前先杀干净旧 msm_webui
- webui 播种仅在启动时执行一次：pending 被外部删除后 `/api/config` 返回 404，
  重启 webui 重新播种即恢复

## 验证
1. simsrc 端到端（2026-08-21）：运行中 → IPC STOP → 写 pending(TrigLength=256) →
   IPC START → 日志 `[main] start: reload config -> configure sim -> start` +
   `[DAQModule] configured: 4ch trigLen=256 frameSize=640`；pending==runtime ✓
2. hwdaq/webui 手动验证（2026-08-24 上午，用户操作）：webui Config 改参数 → 保存 →
   Stop → Start → webui 日志完整走 `start: reload config -> init hw -> start` +
   `[config] pending -> runtime`；pending==runtime（4ch TrigLength=512）；后续 Stop
   flush 正常 ✓
3. 编译：msm_cli_hwdaq / msm_cli_simsrc / msm_webui 全过 ✓
（项目无自动化测试框架，以上为编译 + 冒烟 + 人工验证）

## 日志
- 2026-08-19 创建 task；spec f83e9b1；T2-T5 后端链 + webui pending（8932181→b388cdc）
- 2026-08-19 T6-T7 前端与 config-editor（9fc45a2、f4d9d8b）
- 2026-08-20 T8 gitignore（c04e665）
- 2026-08-21 T8 START→Controller + g_startFn（20baf33）；simsrc 端到端通过
- 2026-08-24 用户 webui 手动验证通过；文档收尾（本提交）

## 结束 commit
代码收尾 20baf33；task 收尾 = 本文件所在 docs 提交
