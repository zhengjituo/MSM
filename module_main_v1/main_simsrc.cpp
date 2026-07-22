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
            g_reload.store(true);
        } else if (msg.cmd == "Quit") {
            g_stop.store(true);
        }
    }
    msm::ModuleVars* getVars() override { return &tableVars_; }
};

static DaqConfig g_config;
static const std::string g_initConfigPath = std::string(MSM_PROJECT_ROOT) + "/module_daq/config/app_initial_config.json";

static void send(const std::string& niString) {
    msm::Message msg = msm::NIStringParser::parse(niString, "Main");
    msm::MessageBus::instance().send(msg);
}

static void distributeConfig() {
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

static void loadConfig() {
    g_config = DaqConfig();
    if (!g_config.loadFromJson(g_initConfigPath)) {
        std::cerr << "[main] Failed to load " << g_initConfigPath << "\n";
        return;
    }
    std::cout << "[main] Config loaded\n";
}

static void printStatus(const ModuleSim& sim,
                        const ModuleProcessFull& process,
                        const ModuleStorage& storage) {
    ModuleSim::Stats s = sim.getStats();
    std::cout << "=== Status ==============================\n"
              << "  SIM: " << (s.is_running ? "生成" : "停止") << "\n";
    for (int i = 0; i < (int)s.total_frames.size(); i++) {
        std::cout << "  ch" << (i + 1)
                  << "  sim_frames=" << s.total_frames[i]
                  << "  proc_frames=" << process.getProcessedFrames(i)
                  << "  proc_batches=" << process.getProcessedBatches(i)
                  << "\n";
    }
    std::cout << "=========================================\n" << std::endl;
}

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    auto& bus = msm::MessageBus::instance();

    loadConfig();

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
        simCfg.setTarget("DAQModule");
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

    auto controller = std::make_shared<ControllerModule>();
    bus.registerModule(controller.get());
    controller->start();

    msm::IPCServer ipc("/tmp/msm_daq.sock");
    bus.registerModule(&ipc);
    ipc.start();

    ModuleProcessFull process("Process", allRings);
    ModuleStorage storage(nullptr, {});
    bus.registerModule(&process);
    bus.registerModule(&storage);
    process.start();

    std::cout << "[main] SIM ready: " << sim.getRingCount() << " rings\n";
    std::cout << "[main] Running. Commands: start / stop / status / reload / quit\n";

    std::thread cli([&]() {
        std::string line;
        while (!g_stop.load() && std::getline(std::cin, line)) {
            if (line.empty()) continue;
            if (line == "quit" || line == "exit") {
                g_stop.store(true);
                break;
            }
            if (line == "start") {
                send("Start >> -@ Process");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                send("Start >> -@ DAQModule");
                continue;
            }
            if (line == "stop") {
                send("Stop >> -@ DAQModule");
                send("Stop >> -@ Process");
                continue;
            }
            if (line == "status") {
                printStatus(sim, process, storage);
                continue;
            }
            if (line == "reload") {
                std::cout << "[main] Reloading...\n";
                send("Stop >> -@ DAQModule");
                send("Stop >> -@ Process");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                loadConfig();
                distributeConfig();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                std::cout << "[main] Reload done. Type 'start' to begin acquisition.\n";
                continue;
            }
            std::cout << "[main] Sending: " << line << std::endl;
            send(line);
        }
    });

    while (!g_stop.load()) {
        if (g_reload.exchange(false)) {
            std::cout << "[main] Reloading (via IPC)...\n";
            send("Stop >> -@ DAQModule");
            send("Stop >> -@ Process");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            loadConfig();
            distributeConfig();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::cout << "[main] Reload done. Type 'start' to begin acquisition.\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\n[main] Shutting down...\n";

    send("Stop >> -@ DAQModule");
    send("Stop >> -@ Process");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cin.setstate(std::ios::eofbit);
    close(STDIN_FILENO);

    ipc.stop();
    sim.stop();
    process.stop();
    storage.stop();

    printStatus(sim, process, storage);
    bus.shutdown();

    if (cli.joinable())
        cli.join();
    return 0;
}
