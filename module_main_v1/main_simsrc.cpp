#include "MessageBus.h"
#include "NIStringParser.h"
#include "IPCServer.h"
#include "module_simsource.h"
#include "module_process_full.h"
#include "module_storage.h"
#include "daq_config.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <memory>
#include <fstream>
#include <sstream>
#include <cmath>

static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_reload{false};
static void handle_signal(int) {
    g_stop.store(true);
}

// Simple controller module: handles Reload/Quit from IPC
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
static const std::string g_initConfigPath = std::string(MSM_PROJECT_ROOT) + "/module_daq/config/app_initial_config.json";
static const std::string g_runtimeConfigPath = std::string(MSM_PROJECT_ROOT) + "/module_daq/config/runtime_config.json";

static void send(const std::string& niString) {
    msm::Message msg = msm::NIStringParser::parse(niString, "Main");
    msm::MessageBus::instance().send(msg);
}

static void distributeConfig() {
    send("Configure >> BatchSize=10000 -@ Process");
    send("Configure >> SavePath=" + g_config.data_save_path
         + ", FileName=" + g_config.file_name
         + ", FileType=" + g_config.file_type
         + ", MaxSaveSizeMB=" + std::to_string(g_config.max_save_size_mb)
         + ", RunId=" + std::to_string(g_config.run_id)
         + ", BatchSize=10000"
         + ", FilePathTemplate={savePath}/{fileName}_s{runId}_ch{ch}_{seq}.{fileType}"
         + " -@ Storage");
}

static void saveConfig() {
    g_config.saveToJson(g_runtimeConfigPath);
}

static void loadAndDistribute() {
    g_config = DaqConfig();
    if (!g_config.loadFromJson(g_initConfigPath)) {
        std::cerr << "[main] Failed to load " << g_initConfigPath << "\n";
        return;
    }
    saveConfig();
    distributeConfig();
    std::cout << "[main] Config loaded and distributed\n";
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

static void writeLiveJSON(const ModuleSim& sim,
                          const ModuleProcessFull& process,
                          const ModuleStorage& storage) {
    ModuleSim::Stats s = sim.getStats();
    int nCh = s.total_frames.size() > 0 ? (int)s.total_frames.size() : 4;
    std::ostringstream out;
    out << "{\n";
    out << "  \"nch\": " << nCh << ",\n";
    out << "  \"running\": " << (s.is_running ? "true" : "false") << ",\n";
    out << "  \"channels\": [\n";
    for (int i = 0; i < nCh; i++) {
        if (i > 0) out << ",\n";
        auto batchData = process.getLastBatchData(i);
        int64_t fcnt = i < (int)s.total_frames.size() ? s.total_frames[i] : process.getProcessedFrames(i);
        out << "    {\n";
        out << "      \"ch\": " << (i + 1) << ",\n";
        out << "      \"frames\": " << fcnt << ",\n";
        out << "      \"overflow\": " << (i < (int)s.overflow_count.size() ? s.overflow_count[i] : 0) << ",\n";
        out << "      \"frame_size\": " << (i < (int)s.frame_size.size() ? s.frame_size[i] : 0) << ",\n";
        out << "      \"processed\": " << process.getProcessedFrames(i) << ",\n";
        out << "      \"batches\": " << process.getProcessedBatches(i) << ",\n";
        out << "      \"trig_id\": " << process.getLastTrigId(i) << ",\n";
        out << "      \"bunch_id\": " << process.getLastBunchId(i) << ",\n";
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
    std::ofstream f("/tmp/msm_live.json");
    f << out.str();
}

static void printStatus(const ModuleSim& sim,
                        const ModuleProcessFull& process,
                        const ModuleStorage& storage) {
    ModuleSim::Stats s = sim.getStats();
    std::cout << "\n=== Status ===\n";
    std::cout << "  SIM running=" << s.is_running << "\n";
    for (int i = 0; i < (int)s.total_frames.size(); i++) {
        std::cout << "  ch" << (i + 1)
                  << ": generated=" << s.total_frames[i]
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

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    auto& bus = msm::MessageBus::instance();

    loadAndDistribute();

    int nCh = 0;
    for (const auto& card : g_config.cards)
        for (const auto& ch : card.channels)
            if (ch.runEnable) nCh++;
    if (nCh == 0) nCh = 4;

    ModuleSim sim;
    bus.registerModule(&sim);
    sim.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    {
        int trigLen = 512;
        const auto* ch1 = g_config.getChannel(1, 1);
        if (ch1) trigLen = std::stoi(ch1->trig_length);

        msm::Message simCfg("Configure");
        simCfg.setTarget("SimDAQ");
        simCfg.setSender("Main");
        simCfg.data.set("NumChannels", nCh);
        simCfg.data.set("TrigLength", trigLen);
        simCfg.data.set("FramesPerCycle", 10000);
        simCfg.data.set("TriggerRateHz", 100000);
        msm::MessageBus::instance().send(simCfg);
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::vector<UserCircularBuffer*> allRings;
    for (int i = 0; i < sim.getRingCount(); i++)
        allRings.push_back(sim.getRing(i));

    ModuleProcessFull process("Process", allRings);
    ModuleStorage storage(nullptr, {});
    auto controller = std::make_shared<ControllerModule>();
    bus.registerModule(controller.get());
    controller->start();

    msm::IPCServer ipc("/tmp/msm_daq.sock");

    bus.registerModule(&process);
    bus.registerModule(&storage);
    bus.registerModule(&ipc);

    process.start();
    ipc.start();
    // Storage 不 start — recording_=false，SaveBatchAll 消息到来时读出丢弃不写盘

    std::cout << "[main] SIM started: " << sim.getRingCount() << " rings\n";

    saveConfig();

    send("Start >> -@ Process");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send("Start >> -@ SimDAQ");

    std::cout << "[main] Running. Commands: quit/status/reload\n";

    std::thread cli([&]() {
        std::string line;
        while (!g_stop.load() && std::getline(std::cin, line)) {
            if (line.empty()) continue;
            if (line == "quit" || line == "exit") {
                g_stop.store(true);
                break;
            }
            if (line == "status") {
                printStatus(sim, process, storage);
                continue;
            }
            if (line == "reload") {
                loadAndDistribute();
                continue;
            }
            std::cout << "[main] Sending: " << line << std::endl;
            send(line);
        }
    });

    writeLiveJSON(sim, process, storage);
    auto lastPrint = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        if (g_reload.exchange(false)) {
            std::cout << "[main] Reloading config...\n";
            send("Stop >> -@ SimDAQ");
            send("Stop >> -@ Process");
            send("Stop >> -@ Storage");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            loadAndDistribute();
            send("Start >> -@ Storage");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            send("Start >> -@ Process");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            send("Start >> -@ SimDAQ");
            std::cout << "[main] Reload done\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto now = std::chrono::steady_clock::now();
        if (now - lastPrint > std::chrono::seconds(2)) {
            auto s = sim.getStats();
            std::cout << "[t="
                      << std::chrono::duration_cast<std::chrono::seconds>(
                             now - lastPrint).count()
                      << "s]";
            for (int i = 0; i < (int)s.total_frames.size(); i++)
                std::cout << " ch" << (i + 1)
                          << "=" << s.total_frames[i];
            std::cout << std::endl;
            lastPrint = now;
        }
        writeLiveJSON(sim, process, storage);
    }

    std::cout << "\n[main] Shutting down...\n";

    send("Stop >> -@ SimDAQ");
    send("Stop >> -@ Process");
    send("Stop >> -@ Storage");

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    ipc.stop();
    sim.stop();
    process.stop();
    storage.stop();

    printStatus(sim, process, storage);
    saveConfig();
    bus.shutdown();

    if (cli.joinable())
        cli.join();
    return 0;
}
