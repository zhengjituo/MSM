# V3 WebUI Dashboard 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use subagent-driven-development or executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**目标:** 将 V1 CLI 交互（start/stop/enablesave/disablesave/帧数据打印）搬到浏览器，module_main_v3 的 WebUI 可用。

**架构:** Process 每满 10k 帧 → 发 BatchReady 消息 → HTTP server（注册为 Module "WebUI"）→ SSE push 到浏览器。WebUI 按钮 → POST /api/msg → HTTP server 处理或转发 MessageBus。全局事件驱动，无定时轮询。

**技术栈:** C++11, httplib (cpp-httplib), MessageBus, SSE (EventSource), 纯 HTML/JS 前端

**文件清单:**

| 文件 | 操作 |
|------|------|
| `module_process/module_process.h` | 改：新增 frameIndex_、setFrameIndex、getFrameIndex |
| `module_process/module_process.cpp` | 改：handleState 处理 SetFrameIndex、doConsumeOnce 发送 BatchReady |
| `module_storage/module_storage.h` | 改：新增 isSaveEnabled() getter |
| `module_main_v3/main_hwdaq.cpp` | 重写：V1 布线 + httplib + SSE + 事件驱动 |
| `module_main_v3/web/msm_daq_hw.html` | 重写：EventSource + 控制面板 + 表格 |
| `module_main_v3/web/status.html` | 删 |
| `module_main_v3/CMakeLists.txt` | 无需改（已有 httplib 依赖） |

---

### Task 1: Process — 新增 frameIndex_ 和 SetFrameIndex 处理

**文件:**
- 改: `module_process/module_process.h`
- 改: `module_process/module_process.cpp`

- [ ] **Step 1: module_process.h — 添加 frameIndex_ 公有接口**

在 `getLastI() const;` 之后添加：
```cpp
    void setFrameIndex(int idx);
    int  getFrameIndex() const { return frameIndex_.load(); }
```

在 `std::string storageTarget_` 之后添加私有字段：
```cpp
    std::atomic<int> frameIndex_{0};
```

- [ ] **Step 2: module_process.cpp — 添加 setFrameIndex 实现**

在 `setStorageTarget` 之后添加：
```cpp
void ModuleProcess::setFrameIndex(int idx) {
    frameIndex_ = idx;
    std::cout << "[" << name_ << "] FrameIndex set to " << idx << "\n";
}
```

- [ ] **Step 3: module_process.cpp — handleState 处理 SetFrameIndex**

在 `handleState` 的 `} else if (msg.cmd == "Status")` 块之后添加：
```cpp
    } else if (msg.cmd == "SetFrameIndex") {
        int idx = 0;
        if (msg.data.contains("index"))
            idx = static_cast<int>(msg.data.getInt("index"));
        setFrameIndex(idx);
```

---

### Task 2: Process — doConsumeOnce 提取所选帧 + 发送 BatchReady

**文件:**
- 改: `module_process/module_process.cpp`

需要在文件顶添加 include：
```cpp
#include "NIStringParser.h"
```

- [ ] **Step 1: doConsumeOnce — 在通道循环内提取 frame index 对应的帧**

修改通道主循环（当前 doConsumeOnce 的 lines 328-362）。在 `if (saveRing_) saveRing_->writeFromDevice(batchBuf_.data(), got);` 行之后、手动读取首帧之前，添加所选帧提取：

```cpp
        // 转发原始数据到 saveRing
        if (saveRing_) saveRing_->writeFromDevice(batchBuf_.data(), got);

        // 提取用户所选 index 的帧
        int selIdx = frameIndex_.load();
        if (selIdx >= 0 && (size_t)selIdx < batchSize_) {
            const uint8_t* frame = batchBuf_.data() + (size_t)selIdx * flen;
            extractFrame(frame, flen, i, 0);
            batchData_[i].count = 1;
            batchData_[i].batchNo = curBatch;
        }

        // 读取首帧用于 CLI 打印
        size_t cnt = 0;
```

注意：`extractFrame` 已经是私有方法。提取的帧数据存到 `batchData_[i]` 的 index 0，SSE 推送时通过 `getLastBatchData(i)` 读到 frames[0] 的 trig_id/bunch_id。

- [ ] **Step 2: doConsumeOnce — 发送 BatchReady 消息**

在 `processedBatches_[i].fetch_add(1);` 循环之后（lines 367-370），添加：

```cpp
    // 通知 WebUI 新 batch 就绪
    {
        std::string body = "BatchReady >> batchNo=" + std::to_string(curBatch)
                         + ", nCh=" + std::to_string(nCh_)
                         + " -@ WebUI";
        msm::Message readyMsg = msm::NIStringParser::parse(body, name_);
        msm::MessageBus::instance().send(readyMsg);
    }
```

---

### Task 3: Storage — 新增 isSaveEnabled getter

**文件:**
- 改: `module_storage/module_storage.h`

- [ ] **Step 1: 添加公有方法声明**

在 `int64_t getSavedBytes(int ch) const;` 之后添加：
```cpp
    bool isSaveEnabled() const { return saveEnabled_.load(); }
```

---

### Task 4: main_hwdaq.cpp — 完整重写

**文件:**
- 改: `module_main_v3/main_hwdaq.cpp`

完整重写。核心结构：

```cpp
#include "MessageBus.h"
#include "NIStringParser.h"
#include "IPCServer.h"
#include "module_daq.h"
#include "module_process.h"
#include "module_storage.h"
#include "daq_config.h"
#include <httplib.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <memory>
#include <fstream>
#include <sstream>
#include <condition_variable>
#include <mutex>
#include <vector>

static std::atomic<bool> g_stop{false};
static std::mutex g_shutdownMtx;
static std::condition_variable g_shutdownCv;

static void handle_signal(int) {
    g_stop.store(true);
    g_shutdownCv.notify_one();
}

static DaqConfig g_config;

static void send(const std::string& niString) {
    msm::Message msg = msm::NIStringParser::parse(niString, "Main");
    msm::MessageBus::instance().send(msg);
}

static void loadConfig() {
    g_config = DaqConfig();
    std::string path = std::string(MSM_PROJECT_ROOT) + "/module_daq/config/app_initial_config.json";
    if (!g_config.loadFromJson(path)) {
        std::cerr << "[main] Failed to load " << path << "\n";
    }
}

static void printStatus(const ModuleDAQ& daq,
                        const ModuleProcess& process,
                        const ModuleStorage& storage) {
    ModuleDAQ::Stats s = daq.getStats();
    std::cout << "=== Status ===\n";
    for (int i = 0; i < (int)s.total_frames.size(); i++) {
        std::cout << "  ch" << (i + 1)
                  << "  daq=" << s.total_frames[i]
                  << "  proc=" << process.getProcessedFrames(i)
                  << "  saved=" << storage.getSavedFrames(i)
                  << "  trig_id=" << process.getLastTrigId(i)
                  << "  bunch_id=" << process.getLastBunchId(i)
                  << "\n";
    }
    std::cout << "==============\n";
}

// SSE 推送数据封装
struct SSEJsonBuilder {
    static std::string build(const ModuleDAQ& daq,
                             const ModuleProcess& process,
                             const ModuleStorage& storage) {
        ModuleDAQ::Stats dstat = daq.getStats();
        int nCh = std::max((int)dstat.total_frames.size(), daq.getRingCount());
        if (nCh == 0) nCh = 4;
        std::ostringstream out;
        out << "data: {";
        out << "\"t\":" << std::time(nullptr) << ",";
        out << "\"running\":" << (dstat.is_running ? "true" : "false") << ",";
        out << "\"save_enabled\":" << (storage.isSaveEnabled() ? "true" : "false") << ",";
        out << "\"frame_index\":" << process.getFrameIndex() << ",";
        out << "\"channels\":[";
        for (int i = 0; i < nCh; i++) {
            if (i > 0) out << ",";
            auto bd = process.getLastBatchData(i);
            uint32_t tid = (bd.count > 0 && !bd.trig_id.empty()) ? bd.trig_id[0] : 0;
            uint64_t bid = (bd.count > 0 && !bd.bunch_id.empty()) ? bd.bunch_id[0] : 0;
            int64_t fcnt = i < (int)dstat.total_frames.size() ? dstat.total_frames[i] : 0;
            
            out << "{";
            out << "\"ch\":" << (i+1) << ",";
            out << "\"daq_frames\":" << fcnt << ",";
            out << "\"processed\":" << process.getProcessedFrames(i) << ",";
            out << "\"batches\":" << process.getProcessedBatches(i) << ",";
            out << "\"saved\":" << storage.getSavedFrames(i) << ",";
            out << "\"trig_id\":" << tid << ",";
            out << "\"bunch_id\":" << bid;
            out << "}";
        }
        out << "]}\n\n";
        return out.str();
    }
};

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    auto& bus = msm::MessageBus::instance();

    loadConfig();

    ModuleDAQ daq;
    bus.registerModule(&daq);
    daq.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Init DAQ
    {
        msm::Message daqMsg("Init");
        daqMsg.data.data = g_config.toKeyValues();
        daqMsg.setTarget("DAQModule");
        daqMsg.setSender("Main");
        bus.send(daqMsg);
    }
    while (!daq.isConfigured())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::vector<UserCircularBuffer*> allRings;
    for (int i = 0; i < daq.getRingCount(); i++)
        allRings.push_back(daq.getRing(i));

    UserCircularBuffer saveRing(64 * 1024 * 1024, 4096);

    std::vector<int> physCh;
    for (int i = 0; i < daq.getRingCount(); i++)
        physCh.push_back(i + 1);

    ModuleProcess process("Process", allRings, &saveRing, physCh);
    ModuleStorage storage(&saveRing, physCh);
    bus.registerModule(&process);
    bus.registerModule(&storage);
    process.start();
    storage.start();

    // Distribute config
    send("Configure >> BatchSize=10000, StorageTarget=Storage -@ Process");
    send("Configure >> SavePath=" + g_config.data_save_path
         + ", FelName=" + g_config.fel_name
         + ", SystemName=" + g_config.system_name
         + ", DeviceName=" + g_config.device_name
         + ", FileType=" + g_config.file_type
         + ", MaxSaveSizeMB=" + std::to_string(g_config.max_save_size_mb)
         + ", ExpNo=" + std::to_string(g_config.exp_no)
         + ", RunSerial=" + g_config.run_serial
         + ", BatchSize=10000"
         + ", FrameSize=" + std::to_string(g_config.frame_size)
         + ", Slot=" + std::to_string(g_config.slot)
         + " -@ Storage");

    while (!daq.isHwReady())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "[main] DAQ ready: " << daq.getRingCount() << " rings\n";

    // IPC server (for external tools)
    msm::IPCServer ipc("/tmp/msm_daq.sock");
    bus.registerModule(&ipc);
    ipc.start();

    // ── HTTP Server (WebUI) ─────────────────────────────────
    httplib::Server httpSvr;

    // SSE state
    std::string sseJson;
    std::mutex sseMtx;
    std::condition_variable sseCv;
    bool sseDataReady = false;
    bool sseShutdown = false;

    // Module "WebUI" — receives BatchReady/Reload/Quit
    class WebUIModule : public msm::Module {
    public:
        WebUIModule(httplib::Server& svr, ModuleDAQ& d, ModuleProcess& p, ModuleStorage& s,
                    std::string& j, std::mutex& m, std::condition_variable& cv,
                    bool& ready, bool& shutdown)
            : Module("WebUI"), httpSvr_(svr), daq_(d), process_(p), storage_(s),
              sseJson_(j), sseMtx_(m), sseCv_(cv), sseDataReady_(ready), sseShutdown_(shutdown) {}

        void handleState(const msm::Message& msg) override {
            if (msg.cmd == "BatchReady") {
                std::string json = SSEJsonBuilder::build(daq_, process_, storage_);
                {
                    std::lock_guard<std::mutex> lock(sseMtx_);
                    sseJson_ = json;
                    sseDataReady_ = true;
                }
                sseCv_.notify_all();

            } else if (msg.cmd == "Reload") {
                std::cout << "[WebUI] Reload triggered\n";
                send("Stop >> -@ DAQModule");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                send("Stop >> -@ Process");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                send("Stop >> -@ Storage");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                loadConfig();
                {
                    msm::Message daqMsg("Init");
                    daqMsg.data.data = g_config.toKeyValues();
                    daqMsg.setTarget("DAQModule");
                    daqMsg.setSender("Main");
                    msm::MessageBus::instance().send(daqMsg);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                send("Configure >> BatchSize=10000, StorageTarget=Storage -@ Process");
                send("Configure >> SavePath=" + g_config.data_save_path
                     + ", FelName=" + g_config.fel_name
                     + ", SystemName=" + g_config.system_name
                     + ", DeviceName=" + g_config.device_name
                     + ", FileType=" + g_config.file_type
                     + ", MaxSaveSizeMB=" + std::to_string(g_config.max_save_size_mb)
                     + ", ExpNo=" + std::to_string(g_config.exp_no)
                     + ", RunSerial=" + g_config.run_serial
                     + ", BatchSize=10000"
                     + ", FrameSize=" + std::to_string(g_config.frame_size)
                     + ", Slot=" + std::to_string(g_config.slot)
                     + " -@ Storage");
                std::cout << "[WebUI] Reload complete\n";

            } else if (msg.cmd == "Quit") {
                std::cout << "[WebUI] Quit triggered\n";
                g_stop.store(true);
                sseShutdown_ = true;
                sseCv_.notify_all();
                httpSvr_.stop();
            }
        }
        msm::ModuleVars* getVars() override { return &tableVars_; }
    private:
        httplib::Server& httpSvr_;
        ModuleDAQ& daq_;
        ModuleProcess& process_;
        ModuleStorage& storage_;
        std::string& sseJson_;
        std::mutex& sseMtx_;
        std::condition_variable& sseCv_;
        bool& sseDataReady_;
        bool& sseShutdown_;
    };

    auto webuiMod = std::make_shared<WebUIModule>(
        httpSvr, daq, process, storage,
        sseJson, sseMtx, sseCv, sseDataReady, sseShutdown);
    bus.registerModule(webuiMod.get());
    webuiMod->start();

    // HTTP API: POST /api/msg
    httpSvr.Post("/api/msg", [&](const httplib::Request& req, httplib::Response& res) {
        std::string cmd = req.body;
        if (cmd.empty()) {
            res.set_content("{\"status\":\"empty\"}", "application/json");
            return;
        }

        // Handle multi-step commands
        if (cmd == "start") {
            send("Start >> -@ Storage");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            send("Start >> -@ Process");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            send("Start >> -@ DAQModule");
            res.set_content("{\"status\":\"started\"}", "application/json");
        } else if (cmd == "stop") {
            send("Stop >> -@ DAQModule");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            send("Stop >> -@ Process");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            send("Stop >> -@ Storage");
            res.set_content("{\"status\":\"stopped\"}", "application/json");
        } else if (cmd == "reload") {
            // WebUIModule handles reload via message
            msm::Message reloadMsg("Reload");
            reloadMsg.setTarget("WebUI");
            reloadMsg.setSender("HTTP");
            bus.send(reloadMsg);
            res.set_content("{\"status\":\"reloading\"}", "application/json");
        } else if (cmd == "quit") {
            msm::Message quitMsg("Quit");
            quitMsg.setTarget("WebUI");
            quitMsg.setSender("HTTP");
            bus.send(quitMsg);
            res.set_content("{\"status\":\"quitting\"}", "application/json");
        } else {
            // Forward to MessageBus as-is
            send(cmd);
            res.set_content("{\"status\":\"ok\"}", "application/json");
        }
    });

    // HTTP API: GET /api/status (initial load / manual refresh)
    httpSvr.Get("/api/status", [&](const httplib::Request&, httplib::Response& res) {
        std::string json = SSEJsonBuilder::build(daq, process, storage);
        res.set_content(json.substr(6), "application/json"); // strip "data: " prefix
    });

    // HTTP API: GET /api/events (SSE)
    httpSvr.Get("/api/events", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content_provider(
            "text/event-stream",
            [&](size_t, httplib::DataSink& sink) -> bool {
                std::unique_lock<std::mutex> lock(sseMtx);
                sseCv_.wait(lock, [&] { return sseDataReady || sseShutdown; });
                if (sseShutdown) return false;
                sink.os << sseJson_;
                sink.os.flush();
                sseDataReady = false;
                return true;
            });
    });

    // Serve dashboard
    httpSvr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream f("web/msm_daq_hw.html");
        if (!f.is_open()) { res.status = 404; res.set_content("not found", "text/plain"); return; }
        std::string html((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        res.set_content(html, "text/html");
    });

    // Start HTTP server in background thread
    std::thread httpThread([&]() {
        std::cout << "[main] WebUI: http://localhost:8080\n";
        httpSvr.listen("0.0.0.0", 8080);
    });

    // Wait for shutdown (event-driven, no timer)
    {
        std::unique_lock<std::mutex> lock(g_shutdownMtx);
        g_shutdownCv.wait(lock, [] { return g_stop.load(); });
    }

    std::cout << "\n[main] Shutting down...\n";

    send("Stop >> -@ DAQModule");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send("Stop >> -@ Process");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send("Stop >> -@ Storage");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    send("CloseHW >> -@ DAQModule");

    httpSvr.stop();
    httpThread.join();

    ipc.stop();
    daq.stop();
    process.stop();
    storage.stop();
    webuiMod->stop();

    printStatus(daq, process, storage);
    bus.shutdown();
    return 0;
}
```

(完整代码，无需改动)

注意：需要重建 `config.h` 和 `daq_config.h` 引用路径——V3 CMakeLists 的 include 路径已包含 `module_core/extern/include` 和 `sdk_include`。

---

### Task 5: WebUI HTML — 重写 dashboard

**文件:**
- 重写: `module_main_v3/web/msm_daq_hw.html`

- [ ] **Step 1: 完整 HTML**

```html
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>MSM DAQ</title>
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }
body { font-family: 'Segoe UI', system-ui, sans-serif; background: #1a1a2e; color: #e0e0e0; padding: 20px; font-size: 14px; }
h1 { color: #00d4ff; font-size: 22px; margin-bottom: 16px; display: flex; align-items: center; gap: 12px; }
.indicator { display: inline-block; width: 10px; height: 10px; border-radius: 50%; margin-right: 4px; }
.indicator.on { background: #4ae0a0; box-shadow: 0 0 6px #4ae0a0; }
.indicator.off { background: #555; }
.btn { padding: 6px 16px; border-radius: 4px; font-size: 13px; font-weight: 600; cursor: pointer; border: none; }
.btn-start { background: #00d4ff; color: #111; }
.btn-stop { background: #e04a4a; color: #fff; }
.btn-reload { background: #f0a030; color: #111; }
.btn:disabled { opacity: 0.4; cursor: default; }
.toolbar { display: flex; gap: 10px; align-items: center; margin-bottom: 14px; flex-wrap: wrap; }
.toolbar label { color: #a0a0b0; font-size: 13px; }
.toolbar input[type=checkbox] { width: 16px; height: 16px; }
.toolbar input[type=number] { width: 70px; padding: 4px 6px; border-radius: 4px; border: 1px solid #0f3460; background: #0a0a1a; color: #4ae0a0; font-size: 13px; }
table { width: 100%; border-collapse: collapse; font-size: 13px; }
th, td { padding: 5px 10px; text-align: right; border-bottom: 1px solid #0f3460; font-variant-numeric: tabular-nums; }
th { color: #a0a0b0; font-weight: 500; text-align: center; background: #16213e; }
td { color: #4ae0a0; }
td:first-child, th:first-child { text-align: left; color: #e0e0e0; }
#statusBar { margin-top: 12px; font-size: 12px; color: #606080; display: flex; gap: 16px; align-items: center; }
#toast { position: fixed; bottom: 20px; right: 20px; background: #0a0a1a; border: 1px solid #00d4ff; border-radius: 6px; padding: 8px 14px; font-size: 12px; color: #00d4ff; opacity: 0; transition: opacity .3s; pointer-events: none; }
#toast.show { opacity: 1; }
</style>
</head>
<body>

<h1>
  MSM DAQ
  <span id="indDaq" class="indicator off"></span>
  <span id="indProc" class="indicator off"></span>
  <span id="indSave" class="indicator off"></span>
  <span style="font-size:12px;color:#606080" id="fpsDisplay">0 fps</span>
</h1>

<div class="toolbar">
  <label><input type="checkbox" id="saveCheck" onchange="toggleSave()"> Save</label>
  <button class="btn btn-start" id="btnStart" onclick="doStart()">▶ Start</button>
  <button class="btn btn-stop" id="btnStop" onclick="doStop()">■ Stop</button>
  <button class="btn btn-reload" onclick="doReload()">↻ Reload</button>
  <span style="width:1px;height:20px;background:#0f3460;"></span>
  <label>Frame index: <input type="number" id="frameIndex" value="0" min="0" max="9999" onchange="setFrameIndex()"></label>
</div>

<table>
  <thead><tr><th>Ch</th><th>DAQ帧</th><th>Process</th><th>Batches</th><th>Saved</th><th>TrigID</th><th>BunchID</th></tr></thead>
  <tbody id="chTable"></tbody>
</table>

<div id="statusBar">
  <span id="statusText">connecting...</span>
  <span id="batchText"></span>
  <span id="updateTime"></span>
</div>
<div id="toast"></div>

<script>
'use strict';

let evtSource = null;
let prevFrames = [];
let prevTime = Date.now();

function toast(msg) {
  const t = document.getElementById('toast');
  t.textContent = msg; t.className = 'show';
  setTimeout(() => t.className = '', 2000);
}

function connectSSE() {
  if (evtSource) evtSource.close();
  evtSource = new EventSource('/api/events');
  evtSource.onmessage = function(e) {
    try {
      const d = JSON.parse(e.data);
      render(d);
    } catch(e) { /* ignore parse errors */ }
  };
  evtSource.onerror = function() {
    document.getElementById('statusText').textContent = 'disconnected';
    document.getElementById('statusText').style.color = '#e04a4a';
    setTimeout(connectSSE, 2000);
  };
}

function render(d) {
  const now = Date.now();
  document.getElementById('indDaq').className = 'indicator ' + (d.running ? 'on' : 'off');
  document.getElementById('indProc').className = 'indicator ' + (d.channels.some(c => c.processed > 0) ? 'on' : 'off');
  document.getElementById('indSave').className = 'indicator ' + (d.channels.some(c => c.saved > 0) ? 'on' : 'off');

  document.getElementById('saveCheck').checked = d.save_enabled;
  document.getElementById('saveCheck').disabled = d.running;

  document.getElementById('btnStart').disabled = d.running;
  document.getElementById('btnStop').disabled = !d.running;

  // Sync frame index input (don't override user edit)
  const fi = document.getElementById('frameIndex');
  if (parseInt(fi.value) !== d.frame_index && !fi.dataset.userEdit) {
    fi.value = d.frame_index;
  }

  const tbody = document.getElementById('chTable');
  let html = '';
  let total = 0;
  for (let i = 0; i < d.channels.length; i++) {
    const ch = d.channels[i];
    total += ch.daq_frames || 0;
    html += '<tr><td>CH' + (i+1) + '</td>'
         + '<td>' + (ch.daq_frames || 0).toLocaleString() + '</td>'
         + '<td>' + (ch.processed || 0).toLocaleString() + '</td>'
         + '<td>' + (ch.batches || 0).toLocaleString() + '</td>'
         + '<td>' + (ch.saved || 0).toLocaleString() + '</td>'
         + '<td>' + (ch.trig_id || 0).toLocaleString() + '</td>'
         + '<td>' + (ch.bunch_id || 0).toLocaleString() + '</td></tr>';
  }
  tbody.innerHTML = html;

  // FPS
  const dt = (now - prevTime) / 1000;
  if (dt > 0 && prevFrames.length > 0) {
    const fps = (total - prevFrames.reduce((a,b)=>a+b,0)) / dt;
    document.getElementById('fpsDisplay').textContent = fps.toFixed(0) + ' fps';
  }
  prevFrames = d.channels.map(c => c.daq_frames || 0);
  prevTime = now;

  document.getElementById('statusText').textContent = d.running ? 'running' : 'stopped';
  document.getElementById('statusText').style.color = d.running ? '#4ae0a0' : '#e04a4a';
  document.getElementById('batchText').textContent = d.channels.length + 'ch';
  document.getElementById('updateTime').textContent = new Date().toLocaleTimeString();
}

async function post(body) {
  try {
    const r = await fetch('/api/msg', { method:'POST', body });
    const j = await r.json();
    toast(j.status);
  } catch(e) {
    toast('FAILED');
  }
}

function doStart() { post('start'); }
function doStop() { post('stop'); }
function doReload() { post('reload'); }

function toggleSave() {
  const cb = document.getElementById('saveCheck');
  post(cb.checked ? 'enablesave' : 'disablesave');
}

function setFrameIndex() {
  const fi = document.getElementById('frameIndex');
  fi.dataset.userEdit = '1';
  post('SetFrameIndex >> index=' + fi.value + ' -@ Process');
  setTimeout(() => delete fi.dataset.userEdit, 2000);
}

// Initial load: fetch status
fetch('/api/status').then(r=>r.json()).then(d => {
  render({...d, channels: d.channels || []});
  document.getElementById('frameIndex').value = d.frame_index || 0;
}).catch(() => {});

connectSSE();
</script>
</body>
</html>
```

- [ ] **Step 2: 删除 status.html**

```bash
git rm module_main_v3/web/status.html
```

---

### Task 6: 构建验证

- [ ] **Step 1: 构建**

```bash
cd /home/zkcx/ws/MSM_DAQ/module_main_v3 && mkdir -p build && cd build && cmake .. && make -j$(nproc)
```

- [ ] **Step 2: 确认无编译错误**

Expected: `msm_web_v3` 链接成功。

---

### Task 7: 提交

- [ ] **Step 1: 提交**

```bash
cd /home/zkcx/ws/MSM_DAQ
git add \
  module_process/module_process.h \
  module_process/module_process.cpp \
  module_storage/module_storage.h \
  module_main_v3/main_hwdaq.cpp \
  module_main_v3/web/msm_daq_hw.html \
  module_main_v3/web/status.html \
  docs/superpowers/plans/2026-07-22-webui-dashboard.md
git commit -m "v3 webui: event-driven dashboard with SSE push

- Process: frameIndex_ + SetFrameIndex handler, BatchReady msg on batch complete
- Storage: isSaveEnabled() getter
- main_hwdaq: httplib HTTP server + SSE + V1 start/stop chain, event-driven
- WebUI: EventSource dashboard with frame index input, start/stop/save controls
- Remove status.html (replaced by dashboard)"
```
