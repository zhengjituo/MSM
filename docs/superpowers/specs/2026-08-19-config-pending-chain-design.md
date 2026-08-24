# Config 单链式生效设计（pending → runtime，无 reload）

日期：2026-08-19　状态：**已实施**（分支 feat/config-pending-chain，task 017，
代码收尾 20baf33；实施记录见文末）

## 背景与动机

崩溃现场（2026-08-18 12:53）：hwdaq 在 webui 保存 runtime config 后 11 秒于
`ModuleStorage::rotateFile` 崩溃。根因是 reload 机制在运行中热改配置，叠加
EPICS 自动采集的残留录制状态，导致运行中字符串成员被破坏。

本设计废弃「运行中 reload 热生效」机制，改为**单链式生效**：

- webui 只编辑「待使用配置」（pending），运行中的后端完全不感知；
- 生效交接由后端在**下次启动/首次采集 Start 时**自主完成（pending → runtime）；
- 彻底移除 reload 功能（webui 按钮/API、CLI 命令、IPC RELOAD、热分发）。

## 文件模型

| 文件 | 角色 | 读写方 |
|---|---|---|
| `config/app_initial_config.json` | 出厂模板，仅首次播种 | webui 读取，几乎不动 |
| `config/app_pending_config.json` | 待使用配置（webui 编辑目标） | webui Config 面板读写 |
| `config/app_runtime_config.json` | 后端实际加载的运行配置 | 后端写入（交接时）；后端启动加载 |

## 流程

### 首次运行（无 pending）
webui 检测 `app_pending_config.json` 不存在 → 从 `app_initial_config.json`
复制生成 pending（原子写）。initial 退化为出厂模板。

### 编辑（运行中）
webui Config 面板只读/写 pending。**后端运行中的状态、字符串、文件句柄
完全不受影响**——这是消除崩溃根因的核心。

### 生效（交接）
后端在**每次采集 Start 时**（实施阶段与用户确认的最终语义，比初稿的
「仅启动阶段」更强）：Start 前依次执行
1. `ensureRuntimeConfig`：检测 `app_pending_config.json` 存在 →
   原子覆盖到 `app_runtime_config.json`（tmp + fsync + rename）；
2. `configModule.config().loadFromJson(g_runtimeConfigPath)`；
3. 发 `Init`（hwdaq）/`Configure`（simsrc）重配硬件；
4. Start 三连（DAQ/Process/Storage）。

安全性：Start 仅在硬件未运行时可触发（webui 门控 + 后端 IPC START 同样只在
非采集态到达），因此重配不与采集并发。IPCServer 的 START 分支只转发
`Message("Start")` 给 ControllerModule，由 Controller 调全局 `g_startFn`
执行上述链；CLI stdin start 走同一入口。

### 无 reload
- webui：移除 Reload 按钮、`/api/config/reload`、`reload` msg；
- 后端：移除 `reload`/`g_reload`/IPC RELOAD 分支、CLI reload 命令；
- 不再有运行中 `loadFromJson` + Distribute。

## 改动点

1. **main_cli/main_hwdaq.cpp**：
   - 启动阶段新增 pending→runtime 交接函数（仅 pending 存在时）；
   - 移除 `reload` 分支（CLI 与 g_reload）；
   - 保留 Stop 语义（Stop 先 DAQ/Process/Storage）。
2. **main_cli/main_simsrc.cpp**：同上。
3. **main_webui/main_hwdaq.cpp**：
   - 编辑目标改为 pending；
   - 首次播种：无 pending 时从 initial 生成；
   - 移除 `/api/config/reload`、reload msg 处理、Reload 按钮；
   - 移除 `saveToInit` 双写逻辑（initial 只作模板）。
4. **main_webui/web/msm_daq_hw.html**：移除 Reload 按钮；保存提示改为
   「已保存到待使用配置，下次 Start 生效」。
5. **main_config_editor**：默认编辑目标路径改为 pending。
6. **ConfigModule / config_editor_module**：目标路径参数化。

## 风险与防护

- 交接必须原子写，避免启动时读到半截文件（复用现有 tmp+fsync+rename）。
- 后端交接后若 loadFromJson 失败，应回退 initial 播种并明确报错，
  避免空配置启动。
- `app_initial_config_backup.json` 保留为出厂手动备份，不参与运行。

## 测试

- simsrc：无 pending 启动 → 播种；编辑 pending → 重启 → 生效；
- 运行中编辑 pending → 后端计数不中断（status 轮询正常）；
- 重复 Start 多次，无崩溃、无文件损坏；
- hwdaq 冒烟：同上 + 确认 rotateFile/reload 路径不再可达。

## 实施记录（2026-08-24 收尾）

提交链（feat/config-pending-chain）：f83e9b1(spec) → 8932181(handoff 助手) →
8e5c856(hwdaq) → 4bdd9ef(simsrc) → b388cdc(webui pending+播种) →
9fc45a2(前端去 reload) → f4d9d8b(config-editor 改 pending) → c04e665(gitignore) →
20baf33(START→Controller 全链 + g_startFn + cards 叠加修复)。
task 流水见 `tasks/017_config_pending_chain.md`。

与初稿的差异 / 实施要点：
1. 交接时机升级为「每次 Start 全链」（上文已按最终语义更新）；
2. ModuleDAQ Init 天生支持热重配：hwReady_ 时跳过 initHW 重开、仅 updateConfig
   推参（module_daq.cpp:76-79）；ModuleSim Configure 可重入动态改参——两者是
   start 链可行的前提，无需新增接口；
3. loadFromJsonString 二次加载会叠加 cards，函数开头补 `cards.clear(); slot=0;`；
4. 部署陷阱：httplib 默认 SO_REUSEPORT，新旧 msm_webui 可同时监听 8080 并分流
   请求；换二进制必须先杀干净旧实例；
5. webui 播种仅启动时执行一次，pending 被外部删除后 `/api/config` 404，
   重启 webui 恢复。

验证：
- simsrc 端到端（2026-08-21）：STOP → 写 pending(256) → START →
  日志确认全链 + trigLen=256 生效，pending==runtime ✓
- hwdaq/webui 手动验证（2026-08-24）：Config 改参 → 保存 → Stop → Start →
  日志 `start: reload config -> init hw -> start` + `pending -> runtime`，
  4ch TrigLength=512 生效，Stop flush 正常 ✓
