# Config 面板 scope 按钮 + app_config 类型化渲染 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 恢复 config 面板三个 scope 保存按钮（channel/card/chassis），app_config 按 JSON 类型渲染（bool→checkbox/number→number/string→text），去掉参数与 app 网格的滚动条。

**Architecture:** 纯前端改动（`main_webui/web/msm_daq_hw.html`）。后端 `applyEdit` 已支持三 scope、`/api/config/save` 已按 scope 分发、`/api/config/save-app` 已存在，无需改动。

**Tech Stack:** JS + 内联 HTML（无框架），httplib 静态资源。

## Global Constraints

- 只提交源码（.html/.cpp/CMakeLists/docs），不提交 build/、二进制、运行时文件（AGENTS.md）
- 跨模块接口/配置格式/数据存储改动需先确认——本计划为纯前端 UI，不涉及（spec 已确认）
- 面板可任何状态打开；保存永不因采集被禁（仅 reload 由前端门控）
- app_config 跳过 `ConfigVersion`（只读，系统维护）
- scope 语义：channel=当前通道；card=当前板卡全部通道；chassis=机箱全部 1073 通道（后端已跳过 3010 时钟卡）

---

### Task 1: 三个 scope 保存按钮

**Files:**
- Modify: `main_webui/web/msm_daq_hw.html:116`（btnCfgSave）与 `:479-487`（doCfgSave）

**Interfaces:**
- Consumes: 端点 `POST /api/config/save` body `{scope, cardIndex, channel, params}`（Task 4 已实现）；
  `cfgCollectParams()`（返回字符串值 map，含 runEnable）已存在于 `:470-477`
- Produces: 三个按钮 `btnCfgSaveChannel`/`btnCfgSaveCard`/`btnCfgSaveChassis`；
  函数 `cfgSave(scope)` 与 `cfgSaveChannel()`/`cfgSaveCard()`/`cfgSaveChassis()`（内联箭头包装）

- [ ] **Step 1: 替换保存按钮**

在 `main_webui/web/msm_daq_hw.html:116` 将：
```html
<button class="btn btn-primary btn-sm" id="btnCfgSave" onclick="doCfgSave()">保存</button>
```
替换为：
```html
<button class="btn btn-primary btn-sm" onclick="cfgSave('channel')">保存当前通道</button>
<button class="btn btn-primary btn-sm" onclick="cfgSave('card')">保存本卡全部通道</button>
<button class="btn btn-primary btn-sm" onclick="cfgSave('chassis')">保存机箱全部 1073 通道</button>
```

- [ ] **Step 2: 重构 doCfgSave → cfgSave(scope)**

在 `:479-487` 将：
```js
async function doCfgSave() {
  const card = cfgCurrentCard();
  const key = document.getElementById('cfgSelChannel').value;
  const channel = key ? parseInt(key.replace('Channel#', ''), 10) : 0;
  const body = { scope: 'channel', cardIndex: card.CardIndex, channel, params: cfgCollectParams() };
  const { r, j } = await fetchJSON('/api/config/save', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  if (j.status === 'ok') { toast('已保存，reload 后生效'); cfgLoad(); }
  else toast('保存失败: ' + (j.status || r.status));
}
```
替换为：
```js
async function cfgSave(scope) {
  const card = cfgCurrentCard();
  if (!card) { toast('请先选择板卡'); return; }
  const key = document.getElementById('cfgSelChannel').value;
  const channel = key ? parseInt(key.replace('Channel#', ''), 10) : 0;
  const body = { scope, cardIndex: card.CardIndex, channel, params: cfgCollectParams() };
  const { r, j } = await fetchJSON('/api/config/save', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  if (j.status === 'ok') { toast('已保存，reload 后生效'); cfgLoad(); }
  else toast('保存失败: ' + (j.status || r.status));
}
```

- [ ] **Step 3: 编译 + 前端验证**

```bash
cd /home/zkcx/ws/MSM_DAQ/build && make msm_webui -j4 2>&1 | tail -4
cd /home/zkcx/ws/MSM_DAQ/main_webui/build && ./msm_webui >/tmp/opencode/t1.log 2>&1 &
sleep 1
wget -q -O - http://localhost:8080/ 2>/dev/null | grep -c "cfgSave('card')\|cfgSave('chassis')\|cfgSave('channel')"
# 预期 ≥3；然后清理：确认 pid 并 kill，避免残留
```
用 `pgrep -af msm_webui` 拿到 pid 后 kill；检查无残留、无 `msm_cli_simsrc`。

- [ ] **Step 4: 提交**

```bash
cd /home/zkcx/ws/MSM_DAQ && git add main_webui/web/msm_daq_hw.html && git commit -m "feat(webui): config panel channel/card/chassis scope save buttons"
```

---

### Task 2: app_config 类型化渲染 + 保存软件参数按钮

**Files:**
- Modify: `main_webui/web/msm_daq_hw.html:440-468`（cfgRenderApp）与 app 区 `<div id="cfgApp">`（`:130-136`）

**Interfaces:**
- Consumes: 端点 `POST /api/config/save-app` body `{app_config, saveToInit}`（Task 4 已实现）；
  `cfgSaveToInit` checkbox（`:133` 已存在）；`cfgLoad()`（重新加载后 cfgRenderApp 刷新）
- Produces: `cfgRenderApp` 按类型渲染（bool→checkbox/number→number/string→text，跳过 ConfigVersion）；
  `cfgSaveApp()` 函数 + `btnCfgSaveApp` 按钮

- [ ] **Step 1: app 区 HTML 加保存按钮**

在 `main_webui/web/msm_daq_hw.html:133` 的 label（cfgSaveToInit）之后追加：
```html
<button class="btn btn-primary btn-sm" id="btnCfgSaveApp" onclick="cfgSaveApp()">保存软件参数</button>
```

- [ ] **Step 2: 重写 cfgRenderApp 为类型化渲染**

在 `:440-468` 将整个 `cfgRenderApp()` 替换为（对齐旧版 renderAppGrid，跳过 ConfigVersion）：
```js
function cfgRenderApp() {
  const grid = document.getElementById('cfgAppGrid');
  grid.innerHTML = '';
  const app = (CFG && CFG.app_config) || {};
  for (const k in app) {
    if (k === 'ConfigVersion') continue;
    const wrap = document.createElement('div');
    wrap.style.display = 'flex';
    wrap.style.flexDirection = 'column';
    wrap.style.gap = '2px';
    const lb = document.createElement('label');
    lb.textContent = k;
    lb.style.fontSize = '11px';
    lb.style.color = '#a0a0b0';
    const val = app[k];
    let input;
    if (typeof val === 'boolean') {
      input = document.createElement('input');
      input.type = 'checkbox';
      input.checked = val;
      input.dataset.jtype = 'bool';
    } else if (typeof val === 'number' || (typeof val === 'string' && /^-?\d+(\.\d+)?$/.test(val))) {
      input = document.createElement('input');
      input.type = 'number';
      input.step = 'any';
      input.value = String(val);
      input.dataset.jtype = 'number';
    } else {
      input = document.createElement('input');
      input.type = 'text';
      input.value = String(val);
      input.dataset.jtype = 'string';
    }
    input.dataset.key = k;
    input.style.width = '100%';
    input.style.background = '#16213e';
    input.style.color = '#e0e0e0';
    input.style.border = '1px solid #0f3460';
    input.style.borderRadius = '4px';
    input.style.padding = '3px 6px';
    input.style.fontSize = '12px';
    wrap.appendChild(lb);
    wrap.appendChild(input);
    grid.appendChild(wrap);
  }
}
```

- [ ] **Step 3: 新增 cfgSaveApp**

在 `cfgRenderApp` 函数之后追加：
```js
async function cfgSaveApp() {
  const ac = {};
  document.querySelectorAll('#cfgAppGrid input[data-key]').forEach(el => {
    const jt = el.dataset.jtype;
    if (jt === 'bool') ac[el.dataset.key] = el.checked;
    else if (jt === 'number') ac[el.dataset.key] = parseFloat(el.value) || 0;
    else ac[el.dataset.key] = el.value;
  });
  const saveToInit = document.getElementById('cfgSaveToInit').checked;
  const { j } = await fetchJSON('/api/config/save-app', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ app_config: ac, saveToInit }) });
  if (j.status === 'ok') { toast(saveToInit ? '软件参数已保存（含启动配置）' : '软件参数已保存'); cfgLoad(); }
  else toast('保存失败: ' + (j.status || 'error'));
}
```

- [ ] **Step 4: 编译 + 前端验证**

```bash
cd /home/zkcx/ws/MSM_DAQ/build && make msm_webui -j4 2>&1 | tail -4
cd /home/zkcx/ws/MSM_DAQ/main_webui/build && ./msm_webui >/tmp/opencode/t2.log 2>&1 &
sleep 1
wget -q -O - http://localhost:8080/ 2>/dev/null | grep -c "jtype = 'bool'\|btnCfgSaveApp\|cfgSaveApp()"
# 预期 ≥3；清理进程同上
```
再验证 meta 中 app_config 布尔字段类型（后端读取 runtime 配置 JSON 原样返回，故 `autoAcqInEpicsWhenSoftwareStarts` 等为 `true`/`false`）：
```bash
wget -q -O - http://localhost:8080/api/config 2>/dev/null | python3 -c "import sys,json;d=json.load(sys.stdin);print([(k,repr(v)) for k,v in d.get('app_config',{}).items() if isinstance(v,bool)])"
# 预期：列出 autoAcqInEpicsWhenSoftwareStarts/isEpicsControlMode/startStorage 等 bool 项
```

- [ ] **Step 5: 提交**

```bash
cd /home/zkcx/ws/MSM_DAQ && git add main_webui/web/msm_daq_hw.html && git commit -m "feat(webui): typed app_config editing (bool/number/string) with save-app button"
```

---

### Task 3: 移除滚动条 + 最终集成验证

**Files:**
- Modify: `main_webui/web/msm_daq_hw.html:128`（cfgParamGrid）与 `:135`（cfgAppGrid）

**Interfaces:**
- Consumes: Task 1/2 产物
- Produces: 无新接口；行为变化——网格全部展开无滚动条

- [ ] **Step 1: 删除 max-height/overflow**

`:128` 将：
```html
<div id="cfgParamGrid" style="display:grid; grid-template-columns:repeat(2,1fr); gap:6px; max-height:340px; overflow:auto;"></div>
```
替换为：
```html
<div id="cfgParamGrid" style="display:grid; grid-template-columns:repeat(2,1fr); gap:6px;"></div>
```
`:135` 将：
```html
<div id="cfgAppGrid" style="display:grid; grid-template-columns:repeat(3,1fr); gap:6px; max-height:200px; overflow:auto;"></div>
```
替换为：
```html
<div id="cfgAppGrid" style="display:grid; grid-template-columns:repeat(3,1fr); gap:6px;"></div>
```

- [ ] **Step 2: 全量编译**

```bash
cd /home/zkcx/ws/MSM_DAQ/build && make -j4 2>&1 | tail -4; echo "exit=${PIPESTATUS[0]}"
```
预期 exit 0。

- [ ] **Step 3: 集成验证（sim 后端，脚本化，不碰用户环境）**

```bash
cd /home/zkcx/ws/MSM_DAQ/main_webui/build && ./msm_webui >/tmp/opencode/t3.log 2>&1 &
sleep 1
echo "--- 页面含三按钮+保存软件参数+无max-height:"
wget -q -O - http://localhost:8080/ 2>/dev/null | grep -c "cfgSave('card')\|cfgSave('chassis')\|cfgSaveApp()"
wget -q -O - http://localhost:8080/ 2>/dev/null | grep -c "max-height"
echo "--- save(card scope):"
wget -q -O - --post-data='{"scope":"card","cardIndex":1,"channel":0,"params":{"VoltageOffset":"0.1"}}' --header='Content-Type: application/json' http://localhost:8080/api/config/save
echo
echo "--- save-app(bool/number/string 类型保持):"
wget -q -O - --post-data='{"app_config":{"startStorage":true,"maxSaveSizeMB":256,"fileName":"PD"}}' --header='Content-Type: application/json' http://localhost:8080/api/config/save-app
echo
echo "--- 验证类型保留:"
wget -q -O - http://localhost:8080/api/config 2>/dev/null | python3 -c "import sys,json;d=json.load(sys.stdin);ac=d['app_config'];print('startStorage',type(ac['startStorage']).__name__,ac['startStorage']);print('maxSaveSizeMB',type(ac['maxSaveSizeMB']).__name__,ac['maxSaveSizeMB'])"
# 预期 startStorage bool True、maxSaveSizeMB int 256（mergeAppConfig 类型保护把字符串转回原始类型）
```
验证后恢复测试写入的 runtime 值（startStorage 改回 false、maxSaveSizeMB 改回 512）并清理进程：
```bash
# 恢复：用 save-app 写回原值
wget -q -O - --post-data='{"app_config":{"startStorage":false,"maxSaveSizeMB":512}}' --header='Content-Type: application/json' http://localhost:8080/api/config/save-app >/dev/null 2>&1
kill $(pgrep -f "main_webui/build/msm_webui" | head -1) 2>/dev/null; sleep 1
pgrep -af "msm_webui|msm_cli_simsrc" | grep -v pgrep || echo "无残留"
```
说明：chassis 冒烟会写全部 1073 卡通道，此处不执行（避免动 config 全量），scope 分发已通过 card 验证 + 后端单测等价路径覆盖。

- [ ] **Step 4: 提交**

```bash
cd /home/zkcx/ws/MSM_DAQ && git add main_webui/web/msm_daq_hw.html && git commit -m "fix(webui): config panels expand fully without scrollbars"
```

---

### Task 4: 文档收尾

**Files:**
- Modify: `docs/superpowers/specs/2026-08-17-config-panel-scope-buttons-appconfig-types-design.md`（状态更新）

**Interfaces:**
- Consumes: Task 1-3 产物
- Produces: spec 状态记录

- [ ] **Step 1: 更新 spec 状态**

将 `状态：已确认（2026-08-17）` 改为 `状态：已实施（2026-08-17）`。

- [ ] **Step 2: 提交**

```bash
cd /home/zkcx/ws/MSM_DAQ && git add docs/superpowers/specs/2026-08-17-config-panel-scope-buttons-appconfig-types-design.md && git commit -m "docs(spec): mark config-panel scope/types design as implemented"
```

---

## 依赖顺序

Task 1 → Task 2 → Task 3 → Task 4（严格串行，同一文件增量修改）。

## 关键风险与回退

- **回退点**：git commit 前 `git reset --hard <上一个 commit>` 可整体回退
- **类型保持**：前端 number 只发字符串，后端 `mergeAppConfig`（config_editor_module.cpp）有类型保护
  （原值 number/bool 时字符串自动转换，转换失败拒绝保存）——前端 `parseFloat`+`checked` 只是改进显示与本地值，
  真正保证类型的是后端 mergeAppConfig，无需改
- **不碰用户环境**：全部冒烟用独立启动的 webui + 恢复写入值 + kill，不 pkill 用户进程