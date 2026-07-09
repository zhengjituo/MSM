#include "MessageBus.h"
#include "NIStringParser.h"
#include "IPCServer.h"
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

static void printStatus(const ModuleSim& sim,
                        const ModuleProcess& process,
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
                  << " saved=" << storage.getSavedFrames(i) << "\n";
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

    ModuleProcess process("Process", allRings);
    ModuleStorage storage(allRings);
    msm::IPCServer ipc("/tmp/msm_daq_sim.sock");

    bus.registerModule(&process);
    bus.registerModule(&storage);
    bus.registerModule(&ipc);

    process.start();
    storage.start();
    ipc.start();

    std::cout << "[main] SIM started: " << sim.getRingCount() << " rings\n";

    saveConfig();

    send("Start >> -@ Storage");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
