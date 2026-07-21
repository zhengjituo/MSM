#include "MessageBus.h"
#include "NIStringParser.h"
#include "IPCServer.h"
#include "module_daq.h"
#include "module_process_full.h"
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
#include <cmath>
#include <cstdio>

static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_reload{false};
static void handle_signal(int) {
    g_stop.store(true);
}

class ControllerModule : public msm::Module {
public:
    ControllerModule() : Module("Controller") {}
    void handleState(const msm::Message& msg) override {
        if (msg.cmd == "Reload") {
            std::cout << "[Controller] Reload triggered via IPC\n";
            g_reload.store(true);
        } else if (msg.cmd == "Quit") {
            std::cout << "[Controller] Quit triggered via IPC\n";
            g_stop.store(true);
        }
    }
    msm::ModuleVars* getVars() override { return &tableVars_; }
};

static DaqConfig g_config;

static void send(const std::string& niString) {
    msm::Message msg = msm::NIStringParser::parse(niString, "Main");
    msm::MessageBus::instance().send(msg);
}

static void distributeConfig(bool isInit = true) {
    msm::Message daqMsg(isInit ? "Init" : "Configure");
    daqMsg.data.data = g_config.toKeyValues();
    daqMsg.setTarget("DAQModule");
    daqMsg.setSender("Main");
    msm::MessageBus::instance().send(daqMsg);

    send("Configure >> BatchSize=10000, StorageTarget=Storage -@ Process");

    send("Configure >> SavePath=" + g_config.data_save_path
         + ", FileName=" + g_config.file_name
         + ", FileType=" + g_config.file_type
         + ", MaxSaveSizeMB=" + std::to_string(g_config.max_save_size_mb)
         + ", RunId=" + std::to_string(g_config.run_id)
         + ", BatchSize=10000"
         + ", FilePathTemplate={savePath}/{fileName}_s{runId}_ch{ch}_{seq}.{fileType}"
         + " -@ Storage");
}

static void printStatus(const ModuleDAQ& daq,
                        const ModuleProcessFull& process,
                        const ModuleStorage& storage) {
    ModuleDAQ::Stats s = daq.getStats();
    std::cout << "\n=== Status ===\n";
    std::cout << "  DAQ running=" << s.is_running << "\n";
    for (int i = 0; i < (int)s.total_frames.size(); i++) {
        std::cout << "  ch" << (i + 1)
                  << ": frames=" << s.total_frames[i]
                  << " overflow=" << s.overflow_count[i]
                  << " frame_size=" << s.frame_size[i]
                  << " processed=" << process.getProcessedFrames(i)
                  << " saved=" << storage.getSavedFrames(i)
                  << " trig_id=" << process.getLastTrigId(i)
                  << " bunch_id=" << process.getLastBunchId(i)
                  << " sum=" << process.getLastSum(i);
        if (i == 0)
            std::cout << " x=" << process.getLastX()
                      << " y=" << process.getLastY()
                      << " I=" << process.getLastI();
        std::cout << "\n";
    }
    std::cout << "================\n" << std::endl;
}

static void writeArray(std::ostream& os, const std::vector<uint32_t>& arr, size_t maxLen) {
    os << "[";
    size_t step = arr.size() > maxLen ? arr.size() / maxLen : 1;
    for (size_t i = 0; i < arr.size(); i += step) {
        if (i > 0) os << ",";
        os << arr[i];
    }
    os << "]";
}

static void writeArray(std::ostream& os, const std::vector<uint64_t>& arr, size_t maxLen) {
    os << "[";
    size_t step = arr.size() > maxLen ? arr.size() / maxLen : 1;
    for (size_t i = 0; i < arr.size(); i += step) {
        if (i > 0) os << ",";
        os << arr[i];
    }
    os << "]";
}

static void writeArray(std::ostream& os, const std::vector<double>& arr, size_t maxLen) {
    os << "[";
    size_t step = arr.size() > maxLen ? arr.size() / maxLen : 1;
    for (size_t i = 0; i < arr.size(); i += step) {
        if (i > 0) os << ",";
        os << (std::isnan(arr[i]) || std::isinf(arr[i]) ? 0.0 : arr[i]);
    }
    os << "]";
}

static constexpr size_t JSON_MAX_ARRAY = 10000;

static void writeLiveJSON(const ModuleDAQ& daq,
                          const ModuleProcessFull& process,
                          const ModuleStorage& storage) {
    ModuleDAQ::Stats dstat = daq.getStats();
    int nCh = dstat.total_frames.size() > 0 ? (int)dstat.total_frames.size() : daq.getRingCount();
    std::ostringstream out;
    out << "{\n";
    out << "  \"nch\": " << nCh << ",\n";
    out << "  \"running\": " << (dstat.is_running ? "true" : "false") << ",\n";
    out << "  \"channels\": [\n";
    for (int i = 0; i < nCh; i++) {
        if (i > 0) out << ",\n";
        auto batchData = process.getLastBatchData(i);
        if (batchData.count > 0) {
            std::cerr << "[JSON] ch" << (i+1) << " batch=" << batchData.batchNo
                      << " t0=" << (batchData.trig_id.size()>0?batchData.trig_id[0]:0)
                      << " b0=" << (batchData.bunch_id.size()>0?batchData.bunch_id[0]:0)
                      << " b1=" << (batchData.bunch_id.size()>1?batchData.bunch_id[1]:0)
                      << " cnt=" << batchData.count << "\n";
        }
        int64_t fcnt = i < (int)dstat.total_frames.size() ? dstat.total_frames[i] : process.getProcessedFrames(i);
        out << "    {\n";
        out << "      \"ch\": " << (i + 1) << ",\n";
        out << "      \"frames\": " << fcnt << ",\n";
        out << "      \"overflow\": " << (i < (int)dstat.overflow_count.size() ? dstat.overflow_count[i] : 0) << ",\n";
        out << "      \"frame_size\": " << (i < (int)dstat.frame_size.size() ? dstat.frame_size[i] : 0) << ",\n";
        out << "      \"processed\": " << process.getProcessedFrames(i) << ",\n";
        out << "      \"batches\": " << process.getProcessedBatches(i) << ",\n";
        out << "      \"trig_id\": " << process.getLastTrigId(i) << ",\n";
        out << "      \"bunch_id\": " << process.getLastBunchId(i) << ",\n";
        out << "      \"sum\": " << process.getLastSum(i) << ",\n";
        out << "      \"saved\": " << storage.getSavedFrames(i) << ",\n";
        if (batchData.count > 0) {
            out << "      \"batch\": {\n";
            out << "        \"no\": " << batchData.batchNo << ",\n";
            out << "        \"count\": " << batchData.count << ",\n";
            out << "        \"trig_id\": ";
            writeArray(out, batchData.trig_id, JSON_MAX_ARRAY);
            out << ",\n";
            out << "        \"bunch_id\": ";
            writeArray(out, batchData.bunch_id, JSON_MAX_ARRAY);
            out << ",\n";
            out << "        \"sum\": ";
            writeArray(out, batchData.sum, JSON_MAX_ARRAY);
            if (i == 0 && batchData.x.size() > 0) {
                out << ",\n";
                out << "        \"x\": ";
                writeArray(out, batchData.x, JSON_MAX_ARRAY);
                out << ",\n";
                out << "        \"y\": ";
                writeArray(out, batchData.y, JSON_MAX_ARRAY);
                out << ",\n";
                out << "        \"i\": ";
                writeArray(out, batchData.i, JSON_MAX_ARRAY);
            }
            out << "\n      }\n";
        } else {
            out << "      \"batch\": null\n";
        }
        out << "    }";
    }
    out << "\n  ]\n}\n";
    std::ofstream f("/tmp/msm_live.tmp");
    f << out.str();
    f.close();
    std::rename("/tmp/msm_live.tmp", "/tmp/msm_live.json");
}

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    auto& bus = msm::MessageBus::instance();

    if (!g_config.loadFromJson("config/app_initial_config.json")) {
        std::cerr << "[main] Failed to load config\n";
        return 1;
    }

    ModuleDAQ daq;
    bus.registerModule(&daq);
    daq.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    distributeConfig();
    while (!daq.isConfigured())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::vector<UserCircularBuffer*> allRings;
    for (int i = 0; i < daq.getRingCount(); i++)
        allRings.push_back(daq.getRing(i));

    auto controller = std::make_shared<ControllerModule>();
    bus.registerModule(controller.get());
    controller->start();

    ModuleProcessFull process("Process", allRings);
    ModuleStorage storage(allRings);
    msm::IPCServer ipc("/tmp/msm_daq.sock");

    bus.registerModule(&process);
    bus.registerModule(&storage);
    bus.registerModule(&ipc);

    process.start();
    storage.start();
    ipc.start();

    // ── HTTP server (WebUI) ─────────────────────────────────────
    httplib::Server httpSvr;
    std::thread httpThread([&]() {
        httpSvr.Post("/api/msg", [](const httplib::Request& req, httplib::Response& res) {
            msm::Message msg = msm::NIStringParser::parse(req.body, "WebUI");
            msm::MessageBus::instance().send(msg);
            res.set_content("{\"status\":\"ok\"}", "application/json");
        });
        httpSvr.Get("/api/status", [&](const httplib::Request&, httplib::Response& res) {
            std::ostringstream out;
            ModuleDAQ::Stats dstat = daq.getStats();
            int nCh = std::max((int)dstat.total_frames.size(), daq.getRingCount());
            if (nCh == 0) nCh = 4;
            out << "{\n";
            out << "  \"nch\": " << nCh << ",\n";
            out << "  \"daq_running\": " << (dstat.is_running ? "true" : "false") << ",\n";
            out << "  \"channels\": [\n";
            for (int i = 0; i < nCh; i++) {
                if (i > 0) out << ",\n";
                int64_t fcnt = i < (int)dstat.total_frames.size() ? dstat.total_frames[i] : 0;
                out << "    {\n";
                out << "      \"ch\": " << (i + 1) << ",\n";
                out << "      \"frames\": " << fcnt << ",\n";
                out << "      \"overflow\": " << (i < (int)dstat.overflow_count.size() ? dstat.overflow_count[i] : 0) << ",\n";
                out << "      \"frame_size\": " << (i < (int)dstat.frame_size.size() ? dstat.frame_size[i] : 384) << ",\n";
                out << "      \"processed\": " << process.getProcessedFrames(i) << ",\n";
                out << "      \"batches\": " << process.getProcessedBatches(i) << ",\n";
                out << "      \"trig_id\": " << process.getLastTrigId(i) << ",\n";
                out << "      \"bunch_id\": " << process.getLastBunchId(i) << ",\n";
                out << "      \"saved\": " << storage.getSavedFrames(i) << "\n";
                out << "    }";
            }
            out << "\n  ]\n}\n";
            res.set_content(out.str(), "application/json");
        });
        // Serve dashboard HTML at root
        httpSvr.Get("/", [](const httplib::Request&, httplib::Response& res) {
            std::ifstream f("web/msm_daq_hw.html");
            if (!f.is_open()) { res.status = 404; res.set_content("dashboard not found", "text/plain"); return; }
            std::string html((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            res.set_content(html, "text/html");
        });
        httpSvr.listen("0.0.0.0", 8080);
    });
    httpThread.detach();
    std::cout << "[main] WebUI: http://localhost:8080\n";

    // Init 帧内部顺序执行: ReadConfig → InitHW → UpdateConfig
    while (!daq.isHwReady())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "[main] DAQ started: " << daq.getRingCount() << " rings\n";
    std::cout << "[main] Storage not started (recording disabled). Use Rec button in web GUI.\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send("Start >> -@ Process");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send("Start >> -@ DAQModule");

    std::cout << "[main] Running. IPC: /tmp/msm_daq.sock  Web: http://localhost:8080\n";

    writeLiveJSON(daq, process, storage);
    while (!g_stop.load()) {
        if (g_reload.exchange(false)) {
            std::cout << "[main] Reloading config...\n";
            send("Stop >> -@ DAQModule");
            send("Stop >> -@ Process");
            send("Stop >> -@ Storage");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            distributeConfig();  // Init → onReadConfig + initHW(no-op) + updateConfig
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            send("Start >> -@ Process");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            send("Start >> -@ DAQModule");
            std::cout << "[main] Reload done\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        writeLiveJSON(daq, process, storage);
    }

    std::cout << "\n[main] Shutting down...\n";

    send("Stop >> -@ DAQModule");
    send("Stop >> -@ Process");
    send("Stop >> -@ Storage");

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    send("CloseHW >> -@ DAQModule");

    ipc.stop();
    daq.stop();
    process.stop();
    storage.stop();

    printStatus(daq, process, storage);
    bus.shutdown();
    return 0;
}
