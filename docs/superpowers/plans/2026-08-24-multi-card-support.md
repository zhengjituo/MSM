# 多卡采集支持 Implementation Plan（task 018）

> 设计见 `docs/superpowers/specs/2026-08-24-multi-card-support-design.md`

**Goal:** 最多 4 张 1073（16 通道）；打开卡 = 配置 slot ∩ 机箱扫描 slot；缺卡报错。

**Architecture:** 严格 slot 匹配由 `module_daq` 承担；卡片集合变化触发
close+reopen；Init 通过 Response 回传错误；ring 大小由 `app_config.RingSizeMB`
控制；WebUI 波形与配置面板动态化。

**Tech Stack:** C++17（module_daq / main_cli / main_webui / module_config / module_process），
webui 原生 HTML/JS。

## Global Constraints

- 只按配置 slot 精确匹配，不做顺序兜底
- 缺卡/超限/无通道 → Init 返回 error，不采集，进程存活
- 机箱多余卡不 open，仅日志提示
- 通道顺序 = 配置 cards 顺序 × 卡内通道顺序
- 编译验证四个目标：msm_cli_hwdaq / msm_cli_simsrc / msm_ipc_shell / msm_webui

---

### Task 1: module_daq 严格校验 + 交集打开 + 错误响应

**Files:**
- Modify: `module_daq/module_daq.h`
- Modify: `module_daq/module_daq.cpp`

**要点:**
1. `initHW()` 扫描后：
   - 由 `config_.cards` 计算 expected 1073 slots / 3010 slot
   - `expected ⊆ scanned` 校验，失败 → 记录缺失清单，`hwReady_=false`，返回 false
   - 只 open+init expected；多余扫描卡打 INFO 忽略
   - 校验 1073 数量 ≤ 4、3010 数量 ≤ 1
2. Init 处理：
   - 若 `!hwReady_` 或 已打开集合 ≠ expected → `initHW()`
   - 成功 → `updateConfig()`；失败 → 回 `Response(status=error, error=原因)`
   - 成功也回 `Response(status=ok)`
3. 保存最近扫描摘要（1073 slots / 3010 slot），供 Status/WebUI 查询
4. `updateConfig()` 的"slot not found"分支保持防御但正常流程不应命中

### Task 2: main 链路错误传递

**Files:**
- Modify: `main_cli/main_hwdaq.cpp`
- Modify: `main_cli/main_simsrc.cpp`

**要点:**
1. `g_startFn` 中 Init 改 `sendAndWait`（超时 60s），检查 `resp.status`
2. 失败：`last_start_error` 记录原因 + 日志，中止 Start 三连
3. 成功：清除 `last_start_error`，继续 Start
4. simsrc 侧 Configure 同样检查响应（无卡概念，失败主要是配置加载）

### Task 3: RingSizeMB 配置

**Files:**
- Modify: `module_config/config_impl.h/.cpp`
- Modify: `main_cli/main_hwdaq.cpp`、`main_cli/main_simsrc.cpp`
- Modify: `config/app_initial_config.json`（加 `"RingSizeMB": 256`）

**要点:**
1. `ConfigImpl` 增 `ring_size_mb`（`findInt(app,"RingSizeMB",256)`；序列化保留）
2. mains 建 ring 用 `cfg.ring_size_mb`
3. 旧配置无该字段 → 默认 256

### Task 4: Process/Storage 文案去 4 通道硬编码

**Files:**
- Modify: `module_process/module_process.cpp`

**要点:**
1. "Print 4-channel feature table" 与表头按 `nCh_` 动态
2. 检查其它 `4` 字面量是否为通道假设，是则动态化

### Task 5: WebUI 波形动态网格

**Files:**
- Modify: `main_webui/web/msm_daq_hw.html`

**要点:**
1. chart 容器按通道数动态创建（4/8/12/16）
2. 布局：≤4 → 2 列；>4 → 4 列
3. echarts 实例按需初始化 / resize / dispose
4. 通道数变化（换配置）时重建

### Task 6: 配置面板 3 按钮 + 添加板卡

**Files:**
- Modify: `main_webui/web/msm_daq_hw.html`
- Modify: `module_config/config_editor_module.cpp`（AddCard）
- Modify: `main_webui/main_hwdaq.cpp`（/api/config/add-card）

**要点:**
1. 第二个按钮 `保存当前板卡所有通道`（scope=card，后端已有）
2. `AddCard(slot, type=1073)`：复制首张 1073 卡的 4 通道参数模板，
   新 CardIndex、新 slot，追加到 Cards
3. 面板显示机箱实际 slots（来自 DAQ 扫描摘要），便于填写 slot
4. 保存后提示"下次 Start 生效"

### Task 7: 编译 + 仿真多卡验证 + 文档

**要点:**
1. 四目标编译
2. 仿真 2 卡 8 通道 / 4 卡 16 通道：SSE 通道数、特征表、波形、存盘
3. 错误路径：配置 slot 不存在 → 报错不采集
4. 真机（用户在场）：单卡回归 + 双卡
5. 更新 `docs/arch/MSM_DAQ的定义.md`、`docs/modules/module_daq.md`、
   `README.md`、`AGENTS.md`；task 018 收尾
