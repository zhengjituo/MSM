# Config 编辑器合并进 msm_webui Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 `msm_config_editor` 独立进程（8081）的配置编辑能力并入 `msm_webui`（8080）进程，删除进程管理与 acquiring 门控，reload 剥离 Stop 且仅空闲可用。

**Architecture:** `ConfigEditorModule`（msm::Module）以 MessageBus 消息方式在 webui 进程内运行；httplib handler 用 `MessageBus::sendAndWait` 同步发 `SaveChannel/SaveCard/SaveChassis/SaveApp/GetConfig` 消息。webui 进程有独立于 hwdaq socket 的 MessageBus 实例。前端 Config 面板从 config_editor.html 移植进 msm_daq_hw.html。

**Tech Stack:** C++17, cpp-httplib, nlohmann::ordered_json, MessageBus（promise/future 同步应答）, CMake。

## Global Constraints

- C++17（根 CMakeLists 已设）
- 跨模块接口/配置格式/数据存储类改动需先确认（项目 AGENTS.md）——本计划已确认
- 只提交源码（.cpp/.h/.html/CMakeLists.txt/docs），不提交 build/、二进制、运行时文件
- 删除 `main_config_editor` 构建但**保留源码目录**
- 旧文件路径归一化（`/../`）模式：`base + "/../../config/"`（webui exe 位于 `main_webui/build/`）
- reload 语义：剥离 Stop，仅空闲可用，生效路径 `stop → reload → start`

---

### Task 1: 剥离 ConfigEditorModule 的 acquiring 门控与 probe 线程

**Files:**
- Modify: `module_config/config_editor_module.h`
- Modify: `module_config/config_editor_module.cpp`
- Modify: `module_config/CMakeLists.txt`（无需改，仅确认仍编译）

**Interfaces:**
- Consumes: 现有 `ConfigEditorModule`（构造/importConfig/applyEdit/applyAppEdit/applyAppEditToFile/getConfigJson/editableChannelKeys/enumOptions/cardTypeLabels）
- Produces: 精简后的 `ConfigEditorModule`，无 probe/acquiring；`start()` 不再起 probe 线程；`applyEdit` 等不再返回 `"locked"`

- [ ] **Step 1: 修改头文件**

删除以下声明（config_editor_module.h）：
- `bool isAcquiring() const { return acquiring_.load(); }`
- `std::string getAcquiringJson() const;`
- `void probeLoop();`
- `bool probeDaqRunning();`
- `std::atomic<bool> acquiring_{false};`
- `std::atomic<bool> running_{false};`
- `std::thread probeThread_;`
- 头注释中的「门控：内部 ~2s 探测 DAQ IPC」段

保留：`mtx_`、`cfg_`、`loadedPath_`、`start()`/`stop()`/`handleState()`、所有 import/export/query/edit 方法。

- [ ] **Step 2: 修改实现文件**

在 config_editor_module.cpp 中：
- 删除 `#include <sys/socket.h> <sys/un.h> <sys/poll.h>`（仅 probe 使用）
- 删除 `registerVars()` 中 acquiring 声明：`vars_.declare("acquiring", "bool", ...)`（保留 registerVars 空实现或删除调用）
- `start()` 改为：
```cpp
void ConfigEditorModule::start() {
    msm::Module::start();
}
```
- `stop()` 改为：
```cpp
void ConfigEditorModule::stop() {
    msm::Module::stop();
}
```
- `applyEdit` 删除 `if (acquiring_.load()) return "locked";`（原 274 行）
- `applyAppEdit` 删除 `if (acquiring_.load()) return "locked";`（原 348 行）
- `applyAppEditToFile` 删除 `if (acquiring_.load()) return "locked";`（原 364 行）
- 删除整个 `probeDaqRunning()` 与 `probeLoop()` 函数（原 451-488 行）
- `handleState` 删除 `GetLock` 分支（原 405-410 行）
- `getAcquiringJson()` 删除（原 231-233 行）

- [ ] **Step 3: 编译验证**

Run:
```bash
cd /home/zkcx/ws/MSM_DAQ/module_config/build && cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 && make -j4 2>&1 | tail -5
```
Expected: 编译通过，无 undefined reference。

- [ ] **Step 4: 提交**

```bash
cd /home/zkcx/ws/MSM_DAQ && git add module_config/config_editor_module.h module_config/config_editor_module.cpp && git commit -m "refactor(config): strip acquiring gate and probe loop from ConfigEditorModule"
```

---

### Task 2: webui 链接 module_config 并实例化 ConfigEditorModule

**Files:**
- Modify: `main_webui/CMakeLists.txt`
- Modify: `main_webui/main_hwdaq.cpp`
- Modify: `CMakeLists.txt`（根，移除 main_config_editor 构建）

**Interfaces:**
- Consumes: `msm::MessageBus`（`instance()/registerModule/sendAndWait`，见 module_core/include/MessageBus.h:26-27）；`msm::ConfigEditorModule`
- Produces: webui 全局 `ConfigEditorModule g_configEditor` + 已注册到 bus；HTTP handler 可用 `sendAndWait` 与之交互

- [ ] **Step 1: CMake 链接**

修改 `main_webui/CMakeLists.txt`：
```cmake
target_link_libraries(msm_webui PRIVATE module_config cpp-httplib pthread)
```
（module_config 是 add_subdirectory 的库目标，且 PUBLIC 依赖 msm_core；include 路径经 PUBLIC 传递。确认 `module_config` 的 `target_include_directories` PUBLIC 已含自身与 msm_core 头。）

- [ ] **Step 2: 根 CMakeLists 停构建**

修改根 `CMakeLists.txt` 第 28 行：注释或删除 `add_subdirectory(main_config_editor)`：
```cmake
# add_subdirectory(main_config_editor)   # 已合并进 main_webui，保留源码停构建
```

- [ ] **Step 3: 实例化与注册**

在 `main_webui/main_hwdaq.cpp` 的 include 区加入：
```cpp
#include "config_editor_module.h"
#include "MessageBus.h"
#include "Message.h"
```

在 `static AppSnapshot g_snap;` 附近（308 行）加入全局实例：
```cpp
static msm::ConfigEditorModule g_configEditor;
static std::string g_runtimeCfgPath;
static std::string g_initCfgPath;
```

在 `main()` 的 `IPCClient ipc;` 之后（约 442 行）初始化：
```cpp
// Config 编辑器（webui 进程内状态机）
{
    char exe[1024];
    ssize_t len = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    std::string base;
    if (len > 0) {
        exe[len] = 0;
        std::string ep(exe);
        size_t s = ep.rfind('/');
        if (s != std::string::npos) base = ep.substr(0, s);
    }
    std::string cand = base + "/../../config/";
    size_t pos;
    while ((pos = cand.find("/../")) != std::string::npos) {
        if (pos == 0) { cand.erase(0, 3); continue; }
        size_t prev = cand.rfind('/', pos - 1);
        if (prev == std::string::npos) prev = 0;
        cand.erase(prev, pos + 3 - prev);
    }
    g_runtimeCfgPath = cand + "app_runtime_config.json";
    g_initCfgPath = cand + "app_initial_config.json";
    std::string err;
    if (!g_configEditor.importConfig(g_runtimeCfgPath, &err))
        std::cerr << "[webui] config editor init: " << err << "\n";
    msm::MessageBus::instance().registerModule(&g_configEditor);
    g_configEditor.start();
}
```

- [ ] **Step 4: 编译验证**

Run:
```bash
cd /home/zkcx/ws/MSM_DAQ/main_webui/build && cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 && make -j4 2>&1 | tail -8
```
Expected: msm_webui 编译链接成功。

- [ ] **Step 5: 提交**

```bash
cd /home/zkcx/ws/MSM_DAQ && git add main_webui/CMakeLists.txt main_webui/main_hwdaq.cpp CMakeLists.txt && git commit -m "feat(webui): embed ConfigEditorModule state machine in-process"
```

---

### Task 3: 删除编辑器进程管理全链路

**Files:**
- Modify: `main_webui/main_hwdaq.cpp`

**Interfaces:**
- Consumes: 无（纯删除）
- Produces: 不再有 `editor_alive` SSE 字段、无 `/api/config` POST 拉起、无 `/api/config/close`；g_editorPid 相关全部移除

- [ ] **Step 1: 删除后端进程管理**

删除以下函数（main_hwdaq.cpp 142-218 行）：
- `findConfigEditorPath()`
- `launchConfigEditor()`
- `stopConfigEditor()`
- `checkEditorChild()`
- 全局 `static pid_t g_editorPid` / `static bool g_editorAlive`（139-140 行）

- [ ] **Step 2: 删除端点**

删除 `/api/config` POST 拉起 handler（542-568 行）与 `/api/config/close` handler（570-573 行）。

- [ ] **Step 3: 删除 SSE 字段与 poll 调用**

- `parseSnapshot` 中 `snap.editorAlive = g_editorPid > 0 && g_editorAlive;`（381 行）与结构体 `bool editorAlive`（307 行）
- `buildSseData` 中 `"editor_alive"` 输出（397 行）
- poll loop 中 `checkEditorChild();`（643 行）
- main 退出处 `stopConfigEditor();`（671 行）

- [ ] **Step 4: 编译验证**

Run:
```bash
cd /home/zkcx/ws/MSM_DAQ/main_webui/build && make -j4 2>&1 | tail -8
```
Expected: 无 g_editorPid/g_editorAlive/editor_alive 相关编译错误。

- [ ] **Step 5: 提交**

```bash
cd /home/zkcx/ws/MSM_DAQ && git add main_webui/main_hwdaq.cpp && git commit -m "refactor(webui): remove config editor child-process management and editor_alive"
```

---

### Task 4: 新增 config HTTP 端点（meta/get/save/save-app/reload）

**Files:**
- Modify: `main_webui/main_hwdaq.cpp`

**Interfaces:**
- Consumes: `g_configEditor`（importConfig 后 loadedPath_ 就绪）、`MessageBus::sendAndWait(Message&, timeout_ms)`、`IPCClient::send(cmd)`
- Produces: HTTP 端点 `GET /api/config/meta`、`GET /api/config`、`POST /api/config/save`、`POST /api/config/save-app`、`POST /api/config/reload`

- [ ] **Step 1: 新增端点代码**

在 `svr.Get("/api/status", ...)`（591 行）之前插入。参考现有 `/api/msg` handler 模式（`svr.Post("/api/msg", ...)` 449 行）：

```cpp
// ── Config 编辑（webui 进程内状态机）──
svr.Get("/api/config/meta", [](const httplib::Request&, httplib::Response& res) {
    nlohmann::ordered_json meta;
    nlohmann::ordered_json keys = nlohmann::ordered_json::array();
    for (auto& k : msm::ConfigEditorModule::editableChannelKeys()) keys.push_back(k);
    nlohmann::ordered_json enums = nlohmann::ordered_json::object();
    for (auto& [k, v] : msm::ConfigEditorModule::enumOptions()) {
        nlohmann::ordered_json arr = nlohmann::ordered_json::array();
        for (auto& o : v) arr.push_back(o);
        enums[k] = arr;
    }
    nlohmann::ordered_json cards = nlohmann::ordered_json::object();
    for (auto& [k, v] : msm::ConfigEditorModule::cardTypeLabels()) cards[k] = v;
    meta["keys"] = keys;
    meta["enums"] = enums;
    meta["cards"] = cards;
    meta["version"] = g_configEditor.currentVersion();
    meta["runtimeFile"] = g_runtimeCfgPath;
    meta["initFile"] = g_initCfgPath;
    res.set_content(meta.dump(), "application/json");
});

svr.Get("/api/config", [](const httplib::Request&, httplib::Response& res) {
    std::string err;
    if (!g_configEditor.importConfig(g_runtimeCfgPath, &err)) {
        res.status = 404;
        res.set_content("{\"status\":\"load failed\",\"msg\":\"" + err + "\"}", "application/json");
        return;
    }
    res.set_content(g_configEditor.getConfigJson(), "application/json");
});

svr.Post("/api/config/save", [](const httplib::Request& req, httplib::Response& res) {
    try {
        auto j = nlohmann::ordered_json::parse(req.body);
        std::string scope = j.value("scope", "channel");
        int cardIndex = j.value("cardIndex", 0);
        int channel = j.value("channel", 0);
        msm::Message msg(scope == "channel" ? "SaveChannel"
                         : scope == "card" ? "SaveCard" : "SaveChassis");
        msg.setTarget("ConfigEditor");
        msg.setSender("WebUI");
        msg.data.set("cardIndex", cardIndex);
        msg.data.set("channel", channel);
        if (j.contains("params")) {
            for (auto& [k, v] : j["params"].items()) {
                std::string sv = v.is_boolean() ? (v.get<bool>() ? "true" : "false")
                              : v.is_string() ? v.get<std::string>() : v.dump();
                msg.data.set("param." + k, sv);
            }
        }
        msm::Message resp = msm::MessageBus::instance().sendAndWait(msg);
        std::string status = resp.data.get("status", "timeout");
        if (status == "ok") { res.set_content(R"({"status":"ok"})", "application/json"); return; }
        if (status == "timeout") { res.status = 500; res.set_content(R"({"status":"editor timeout"})", "application/json"); return; }
        res.status = 400;
        res.set_content("{\"status\":\"" + status + "\"}", "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(std::string("{\"status\":\"error\",\"msg\":\"") + e.what() + "\"}", "application/json");
    }
});

svr.Post("/api/config/save-app", [](const httplib::Request& req, httplib::Response& res) {
    try {
        auto j = nlohmann::ordered_json::parse(req.body);
        std::string appJson;
        if (j.contains("app_config")) appJson = j["app_config"].dump();
        else appJson = req.body;
        msm::Message msg("SaveApp");
        msg.setTarget("ConfigEditor");
        msg.setSender("WebUI");
        msg.data.set("app_config_json", appJson);
        msm::Message resp = msm::MessageBus::instance().sendAndWait(msg);
        std::string status = resp.data.get("status", "timeout");
        if (status != "ok") {
            res.status = (status == "timeout") ? 500 : 400;
            res.set_content("{\"status\":\"" + status + "\"}", "application/json");
            return;
        }
        bool saveToInit = j.value("saveToInit", false);
        if (saveToInit) {
            std::string st2 = g_configEditor.applyAppEditToFile(appJson, g_initCfgPath);
            if (st2 != "ok") {
                res.status = 400;
                res.set_content("{\"status\":\"" + st2 + "\"}", "application/json");
                return;
            }
        }
        res.set_content(R"({"status":"ok"})", "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(std::string("{\"status\":\"error\",\"msg\":\"") + e.what() + "\"}", "application/json");
    }
});

svr.Post("/api/config/reload", [&](const httplib::Request&, httplib::Response& res) {
    std::string r = ipc.send("RELOAD");
    res.set_content(r, "application/json");
});
```

注意 `ConfigEditorModule` 的 `msg.data.set("cardIndex", int)` 走 int 重载，`getInt` 可读；`param.*` 走字符串重载。

- [ ] **Step 2: 编译验证**

Run:
```bash
cd /home/zkcx/ws/MSM_DAQ/main_webui/build && make -j4 2>&1 | tail -8
```
Expected: 编译通过。若 `nlohmann` include 缺失，在 main_hwdaq.cpp 顶部 `#include <nlohmann/json.hpp>`（webui 已通过 module_core 传递可用）。

- [ ] **Step 3: 运行时冒烟测试（sim）**

Run:
```bash
cd /home/zkcx/ws/MSM_DAQ/main_webui/build && ./msm_webui >/tmp/opencode/webui_merge.log 2>&1 &
sleep 1
curl -s http://localhost:8080/api/config/meta | head -c 300; echo
curl -s http://localhost:8080/api/config | head -c 200; echo
curl -s -X POST http://localhost:8080/api/config/save -H 'Content-Type: application/json' \
  -d '{"scope":"channel","cardIndex":1,"channel":1,"params":{"VoltageOffset":"0.1"}}'; echo
```
Expected: meta 含 keys/enums；config 返回 JSON；save 返回 `{"status":"ok"}`。完成后 kill webui。

- [ ] **Step 4: 提交**

```bash
cd /home/zkcx/ws/MSM_DAQ && git add main_webui/main_hwdaq.cpp && git commit -m "feat(webui): add config edit endpoints backed by in-process state machine"
```

---

### Task 5: reload 剥离 Stop + 空闲门控（后端）

**Files:**
- Modify: `main_cli/main_hwdaq.cpp`

**Interfaces:**
- Consumes: 现有 reload 分支（217-238 行）
- Produces: reload 不再发 Stop；可选前置检查

- [ ] **Step 1: 修改 reload 分支**

将 reload 分支（217-237 行）改为：
```cpp
if (line == "reload") {
    std::cout << "[main] Reloading from runtime config...\n";
    // 剥离 Stop：reload 仅空闲时由前端门控调用；此处直接加载
    if (!configModule.config().loadFromJson(g_runtimeConfigPath)) {
        std::cerr << "[main] Reload failed: cannot load " << g_runtimeConfigPath << "\n";
    } else {
        msm::Message dist("Distribute");
        dist.setTarget("ConfigModule");
        dist.setSender("Main");
        dist.data.set("isInit", false);
        msm::MessageBus::instance().send(dist);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::cout << "[main] Reload done. Type 'start' to begin acquisition.\n";
    continue;
}
```
即删除 4 个 `send("Stop >> -@ ...")` + sleep 序列。

- [ ] **Step 2: 编译验证**

Run:
```bash
cd /home/zkcx/ws/MSM_DAQ/main_cli/build && make -j4 2>&1 | tail -5
```
Expected: 编译通过。

- [ ] **Step 3: 提交**

```bash
cd /home/zkcx/ws/MSM_DAQ && git add main_cli/main_hwdaq.cpp && git commit -m "feat(cli): reload no longer forces stop; gated idle-only by frontend"
```

---

### Task 6: 前端 Config 面板移植 + 门控

**Files:**
- Modify: `main_webui/web/msm_daq_hw.html`

**Interfaces:**
- Consumes: HTTP 端点（Task 4）：`/api/config/meta`、`/api/config`、`/api/config/save`、`/api/config/save-app`、`/api/config/reload`；SSE 字段 `d.running`
- Produces: Config 面板 UI + 保存/reload 逻辑

- [ ] **Step 1: 删除旧按钮与 editor 状态渲染**

从 msm_daq_hw.html 删除：
- `btnConfig`（⚙ Config）与 `btnConfigClose`（✕ Close）按钮（75-76 行）
- `editorIndicator` / `editorStatusText` / `indEditor` 相关（214-217 行渲染）
- `doConfig()` / `doConfigClose()` 函数（302-313 行）
- `editorAlive` 门控（211-217 行）

- [ ] **Step 2: 移植 Config 面板 HTML**

在 HTML 顶部（`<body>` 后，status 区附近）新增面板容器。参考 config_editor.html 的卡片结构，精简移植：
```html
<div class="panel" id="configPanel" style="display:none; border:1px solid var(--border); border-radius:8px; padding:14px; margin-top:12px;">
  <div style="display:flex; gap:10px; align-items:center; flex-wrap:wrap; margin-bottom:10px;">
    <span class="label">Config</span>
    <span id="cfgVersion" style="color:var(--muted); font-size:12px;"></span>
    <button class="btn btn-sm" id="btnCfgSave" onclick="doCfgSave()">保存</button>
    <button class="btn btn-warn btn-sm" id="btnCfgReload" onclick="doCfgReload()">Reload</button>
    <button class="btn btn-default btn-sm" onclick="toggleCfgPanel(false)">关闭</button>
  </div>
  <div id="cfgBody" style="display:grid; grid-template-columns:220px 1fr; gap:12px;">
    <div>
      <select id="cfgSelCard" class="cfg-sel" onchange="cfgPopulateChannels()"></select>
      <select id="cfgSelChannel" class="cfg-sel" onchange="cfgRenderParams()" style="margin-top:6px; width:100%;"></select>
      <label style="display:flex; gap:6px; align-items:center; margin-top:8px; font-size:13px;">
        <input type="checkbox" id="cfgRunEnable"> runEnable
      </label>
    </div>
    <div id="cfgParamGrid" style="display:grid; grid-template-columns:repeat(2,1fr); gap:6px; max-height:340px; overflow:auto;"></div>
  </div>
  <div id="cfgApp" style="margin-top:10px; border-top:1px solid var(--border); padding-top:8px;">
    <div style="display:flex; gap:8px; align-items:center; margin-bottom:6px;">
      <span class="label" style="font-size:12px;">app_config</span>
      <label style="font-size:12px; display:flex; gap:4px;"><input type="checkbox" id="cfgSaveToInit"> 同时写入启动配置</label>
    </div>
    <div id="cfgAppGrid" style="display:grid; grid-template-columns:repeat(3,1fr); gap:6px; max-height:200px; overflow:auto;"></div>
  </div>
</div>
```

- [ ] **Step 3: 移植 JS 逻辑**

在 `<script>` 内加入（含从 config_editor.html 复制的 META/fetch/渲染/保存逻辑，改为 async 串行 meta→cfg 加载）：
```js
let CFG_META = { keys: [], enums: {}, cards: {}, version: '', runtimeFile: '', initFile: '' };
let CFG = null;

async function fetchJSON(url, opts) {
  const r = await fetch(url, opts);
  let j = {};
  try { j = await r.json(); } catch(e) {}
  return { r, j };
}

async function cfgLoadMeta() {
  const { j } = await fetchJSON('/api/config/meta');
  CFG_META = Object.assign(CFG_META, j);
  document.getElementById('cfgVersion').textContent = '版本 ' + CFG_META.version;
}

async function cfgLoad() {
  const { r, j } = await fetchJSON('/api/config');
  if (!r.ok) { toast('配置加载失败'); return; }
  CFG = j;
  cfgPopulateCards();
  cfgRenderApp();
}

function toggleCfgPanel(show) {
  const p = document.getElementById('configPanel');
  p.style.display = show ? 'block' : 'none';
  if (show && !CFG) { cfgLoadMeta().then(() => cfgLoad()); }
}

function cfgCurrentCard() {
  const name = document.getElementById('cfgSelCard').value;
  return (CFG && CFG.DaqParameter && CFG.DaqParameter.Cards && CFG.DaqParameter.Cards[name]) || null;
}

function cfgPopulateCards() {
  const cards = (CFG && CFG.DaqParameter && CFG.DaqParameter.Cards) || {};
  const sel = document.getElementById('cfgSelCard');
  sel.innerHTML = '';
  for (const name in cards) {
    const c = cards[name];
    const label = (CFG_META.cards && CFG_META.cards[c.CardType]) || c.CardType;
    const opt = document.createElement('option');
    opt.value = name;
    opt.textContent = name + ' [' + label + '] (Slot ' + c.Slot + ')';
    sel.appendChild(opt);
  }
  cfgPopulateChannels();
}

function cfgPopulateChannels() {
  const card = cfgCurrentCard();
  const sel = document.getElementById('cfgSelChannel');
  sel.innerHTML = '';
  const chs = (card && card.Channels) || {};
  for (const key in chs) {
    if (!chs[key] || typeof chs[key] !== 'object') continue;
    const opt = document.createElement('option');
    opt.value = key;
    opt.textContent = key + (chs[key].runEnable ? ' ●' : ' ○');
    sel.appendChild(opt);
  }
  cfgRenderParams();
}

function cfgCurrentChannel() {
  const chs = (cfgCurrentCard() || {}).Channels || {};
  const key = document.getElementById('cfgSelChannel').value;
  return key ? chs[key] : null;
}

function cfgRenderParams() {
  const ch = cfgCurrentChannel();
  const grid = document.getElementById('cfgParamGrid');
  grid.innerHTML = '';
  document.getElementById('cfgRunEnable').checked = !!ch && !!ch.runEnable;
  if (!ch) { grid.innerHTML = '<div class="note" style="color:var(--muted)">无可编辑通道</div>'; return; }
  for (const key of CFG_META.keys) {
    if (key === 'cardIndex' || key === 'channel' || key === 'slot' || key === 'runEnable') continue;
    if (ch[key] === undefined) continue;
    grid.appendChild(cfgField(key, ch[key]));
  }
}

function cfgField(key, value) {
  const wrap = document.createElement('div');
  wrap.style.display = 'flex';
  wrap.style.flexDirection = 'column';
  wrap.style.gap = '2px';
  const lb = document.createElement('label');
  lb.textContent = key;
  lb.style.fontSize = '11px';
  lb.style.color = 'var(--muted)';
  let input;
  const opts = CFG_META.enums && CFG_META.enums[key];
  if (opts) {
    input = document.createElement('select');
    for (const o of opts) {
      const op = document.createElement('option');
      op.value = o; op.textContent = o;
      input.appendChild(op);
    }
    input.value = String(value);
  } else {
    const numeric = typeof value === 'number' || /^-?\d+(\.\d+)?$/.test(String(value));
    input = document.createElement('input');
    input.type = numeric ? 'number' : 'text';
    input.step = 'any';
    input.value = String(value);
  }
  input.dataset.key = key;
  input.style.width = '100%';
  wrap.appendChild(lb);
  wrap.appendChild(input);
  return wrap;
}

function cfgRenderApp() {
  const grid = document.getElementById('cfgAppGrid');
  grid.innerHTML = '';
  const app = (CFG && CFG.app_config) || {};
  for (const k in app) {
    const wrap = document.createElement('div');
    wrap.style.display = 'flex';
    wrap.style.flexDirection = 'column';
    wrap.style.gap = '2px';
    const lb = document.createElement('label');
    lb.textContent = k;
    lb.style.fontSize = '11px';
    lb.style.color = 'var(--muted)';
    const input = document.createElement('input');
    input.type = 'text';
    input.value = String(app[k]);
    input.dataset.key = k;
    wrap.appendChild(lb);
    wrap.appendChild(input);
    grid.appendChild(wrap);
  }
}

function cfgCollectParams() {
  const params = {};
  document.querySelectorAll('#cfgParamGrid input[data-key], #cfgParamGrid select[data-key]').forEach(el => {
    params[el.dataset.key] = el.value;
  });
  params.runEnable = document.getElementById('cfgRunEnable').checked ? 'true' : 'false';
  return params;
}

async function doCfgSave() {
  const card = cfgCurrentCard();
  const key = document.getElementById('cfgSelChannel').value;
  const channel = key ? parseInt(key.replace('Channel#', ''), 10) : 0;
  const body = { scope: 'channel', cardIndex: card.CardIndex, channel, params: cfgCollectParams() };
  const { r, j } = await fetchJSON('/api/config/save', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  if (j.status === 'ok') { toast('已保存，reload 后生效'); cfgLoad(); }
  else toast('保存失败: ' + (j.status || r.status));
}

async function doCfgReload() {
  const { j } = await fetchJSON('/api/config/reload', { method: 'POST' });
  toast('reload: ' + (j.status || 'ok'));
}
```

在 `render(d)` 中加入 reload 门控（参考 btnReload 现有逻辑）：
```js
const rld = document.getElementById('btnCfgReload');
if (rld) rld.disabled = !backendRunning || d.running;
```

顶部按钮区新增「Config」按钮，点击 `toggleCfgPanel(true)`：
```html
<button class="btn btn-default btn-sm" id="btnConfigPanel" onclick="toggleCfgPanel(true)">Config</button>
```

- [ ] **Step 4: 前端验证**

Run（webui 已带 sim 后端时）：
```bash
curl -s http://localhost:8080/ | grep -c "cfgLoad\|configPanel\|btnCfgReload"
```
Expected: 输出 ≥3（面板已内嵌）。

- [ ] **Step 5: 提交**

```bash
cd /home/zkcx/ws/MSM_DAQ && git add main_webui/web/msm_daq_hw.html && git commit -m "feat(webui): inline config panel with idle-only reload gating"
```

---

### Task 7: 集成验证与文档收尾

**Files:**
- Modify: `tasks/016_phase_c_webui_entry_disk_safety.md`
- Modify: `docs/superpowers/specs/2026-08-14-config-editor-merge-webui-design.md`（更新状态）
- 根 `CMakeLists.txt`（确认已停构建 main_config_editor）

**Interfaces:**
- Consumes: 全部 Task 1-6 产物
- Produces: 验证记录 + task 完结

- [ ] **Step 1: 全量编译**

Run:
```bash
cd /home/zkcx/ws/MSM_DAQ && rm -rf main_webui/build && mkdir -p main_webui/build && cd main_webui/build && cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 && make -j4 2>&1 | tail -6
```
Expected: msm_webui 构建通过；确认 `main_config_editor/build` 不再生成新产物。

- [ ] **Step 2: 集成测试（sim 模式）**

Run（脚本化，注意**不 pkill 用户环境**，用独立端口场景）：
```bash
cd /home/zkcx/ws/MSM_DAQ/main_webui/build
./msm_webui >/tmp/opencode/merge_e2e.log 2>&1 &
WUI=$!
sleep 1
echo "--- meta:"; curl -s http://localhost:8080/api/config/meta | head -c 200; echo
echo "--- cfg keys:"; curl -s http://localhost:8080/api/config | python3 -c "import sys,json;d=json.load(sys.stdin);print(list(d.get('DaqParameter',{}).keys()))"
echo "--- save:"; curl -s -X POST http://localhost:8080/api/config/save -H 'Content-Type: application/json' -d '{"scope":"channel","cardIndex":1,"channel":1,"params":{"VoltageOffset":"0.1"}}'
echo "--- save-app:"; curl -s -X POST http://localhost:8080/api/config/save-app -H 'Content-Type: application/json' -d '{"app_config":{"SomeKey":"1"}}'
echo "--- reload(无后端应 timeout):"; curl -s -X POST http://localhost:8080/api/config/reload; echo
kill $WUI 2>/dev/null
```
Expected: meta 正常；config 含 ChassisNo/Cards；save/save-app 返回 ok；reload 返回 disconnected/timeout（无后端时预期）。

- [ ] **Step 3: 回归确认（start/stop 不受影响）**

若环境允许（用户在场时）手动回归：Launch → Start → Stop → status 正常；Config 面板打开/保存/Reload（空闲）正常；采集运行中 Reload 按钮禁用。

- [ ] **Step 4: 更新 task 文件**

在 `tasks/016_phase_c_webui_entry_disk_safety.md` 追加：
```markdown
## 阶段 C.5 — config 编辑器合并进 webui（2026-08-14）
- 动机：close 按钮竞态/PID 复用/SIGKILL 兜底、editor_alive 状态错乱、acquiring 门控失效
  （probe 字符串匹配 bug：`"running": "true"` 带引号，`"running":true` 永不匹配）
- 方案：ConfigEditorModule 以 MessageBus 消息并入 webui 进程；删进程管理+门控；
  reload 剥离 Stop，仅空闲可用
- 决策：保留 main_config_editor 源码，停构建；运行中保存只写文件不自动生效
```

- [ ] **Step 5: 提交**

```bash
cd /home/zkcx/ws/MSM_DAQ && git add tasks/016_phase_c_webui_entry_disk_safety.md && git commit -m "docs(task 016): record config-editor merge into webui"
```

---

## 依赖顺序

Task 1 → Task 2 → Task 3 → Task 4 → Task 5 → Task 6 → Task 7

Task 1 必须最先（剥离门控让 Task 4 的 save 永不返回 locked）。Task 2/3 可并行但都在同一文件 main_hwdaq.cpp，建议串行。Task 5 独立于 Task 2-4。Task 7 最后。

## 关键风险与回退

- **回退点**：git commit 前 `git reset --hard <上一个 commit>` 可回到独立进程版本
- **webui 与 hwdaq 双总线**：webui 进程内 MessageBus 只有 ConfigEditorModule；IPCClient 连 socket 发 RELOAD 到 hwdaq 进程——两者互不干扰，已在设计中隔离
- **resolveConfigPath**：webui exe 在 `main_webui/build/`，`base + "/../../config/"` 解析到仓库 `config/`，与 main_config_editor 的解析一致
