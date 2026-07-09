#include "MessageBus.h"
#include "NIStringParser.h"
#include "IPCServer.h"
#include "RESTServer.h"
#include "PVServer.h"
#include "module_sim.h"
#include "module_process.h"
#include "module_storage.h"
#include "daq_config.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

static std::atomic<bool> g_stop{false};
static void handle_signal(int) {
    g_stop.store(true);
}

static DaqConfig g_config;
static const std::string g_initConfigPath = "config/app_inittial_config.json";
static const std::string g_runtimeConfigPath = "config/runtime_config.json";
static const std::string g_pvConfigPath = "pv_config.json";
static const std::string g_restConfigPath = "rest_config.json";

static ModuleSim* g_sim = nullptr;
static ModuleProcess* g_process = nullptr;
static ModuleStorage* g_storage = nullptr;

static void send(const std::string& niString) {
    msm::Message msg = msm::NIStringParser::parse(niString, "Main");
    msm::MessageBus::instance().send(msg);
}

static void distributeConfig() {
    // Config messages are sent after module registration (see main())
}

static void sendStorageConfig() {
    std::string saveEnabled = g_config.start_storage ? "true" : "false";
    send("Configure >> SavePath=" + g_config.data_save_path
         + ", FileName=" + g_config.file_name
         + ", FileType=" + g_config.file_type
         + ", MaxSaveSizeMB=" + std::to_string(g_config.max_save_size_mb)
         + ", RunId=" + std::to_string(g_config.run_id)
         + ", BatchSize=10000"
         + ", SaveEnabled=" + saveEnabled
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

static void printStatus() {
    if (!g_sim || !g_process || !g_storage) return;
    auto s = g_sim->getStats();
    std::cout << "\n=== Status ===\n";
    std::cout << "  SIM running=" << s.is_running << "\n";
    for (int i = 0; i < (int)s.total_frames.size(); i++) {
        std::cout << "  ch" << (i + 1)
                  << ": generated=" << s.total_frames[i]
                  << " overflow=" << s.overflow_count[i]
                  << " frame_size=" << s.frame_size[i]
                  << " processed=" << g_process->getProcessedFrames(i)
                  << " saved=" << g_storage->getSavedFrames(i) << "\n";
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

    // --- Create modules ---
    ModuleSim sim;
    g_sim = &sim;
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

    ModuleProcess process("Process", allRings);
    g_process = &process;
    ModuleStorage storage(allRings);
    g_storage = &storage;
    msm::IPCServer ipc("/tmp/msm_daq_sim.sock");
    msm::RESTServer rest(8080);
    std::string epicsBase = std::string(MSM_PROJECT_ROOT) + "/module_core/extern/epics";
    msm::PVServer pv("MSM");
    pv.enableIOC(epicsBase, "msm_pvs.db");

    bus.registerModule(&process);
    bus.registerModule(&storage);
    bus.registerModule(&ipc);
    bus.registerModule(&rest);
    bus.registerModule(&pv);

    process.start();
    send("Configure >> BatchSize=10000 -@ Process");
    storage.start();
    sendStorageConfig();
    ipc.start();

    // Load PV and REST configs (auto-setup happens in start())
    if (!rest.loadConfig(g_restConfigPath)) {
        std::cerr << "[main] Failed to load " << g_restConfigPath << "\n";
    } else {
        rest.enableStaticDir(".");
        rest.start();
    }

    if (!pv.loadConfig(g_pvConfigPath)) {
        std::cerr << "[main] Failed to load " << g_pvConfigPath << "\n";
    } else {
        pv.start();
        auto pushPV = [&](const std::string& mod) {
            msm::Message m("PV_UPDATE");
            m.data.set("module", mod);
            m.data.set("var", "*");
            m.setTarget("PVServer");
            msm::MessageBus::instance().send(m);
        };
        pushPV("SimDAQ"); pushPV("Process"); pushPV("Storage");
    }

    std::cout << "[main] SIM started: " << sim.getRingCount() << " rings\n";

    saveConfig();

    // Auto-acquire if configured
    if (g_config.auto_acq_on_start) {
        send("Start >> -@ Storage");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        send("Start >> -@ Process");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        send("Start >> -@ SimDAQ");
        std::cout << "[main] Auto acquisition started\n";
    } else {
        std::cout << "[main] Waiting. Run 'Start >> -@ SimDAQ' etc. to begin\n";
    }

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
                printStatus();
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

    auto lastPrint = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    }

    std::cout << "\n[main] Shutting down...\n";

    send("Stop >> -@ SimDAQ");
    send("Stop >> -@ Process");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    send("Stop >> -@ Storage");

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    pv.stop();
    rest.stop();
    ipc.stop();
    sim.stop();
    process.stop();
    storage.stop();

    printStatus();
    saveConfig();
    bus.shutdown();

    if (cli.joinable())
        cli.join();
    return 0;
}
