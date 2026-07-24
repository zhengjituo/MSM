#include "MessageBus.h"
#include "NIStringParser.h"
#include "IPCServer.h"
#include "module_simsource.h"
#include "module_process.h"
#include "module_storage.h"
#include "daq_config.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <memory>
#include <fstream>
#include <iomanip>

static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_reload{false};
static void handle_signal(int) {
    g_stop.store(true);
}

static void send(const std::string& niString) {
    msm::Message msg = msm::NIStringParser::parse(niString, "Main");
    msm::MessageBus::instance().send(msg);
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

static void distributeConfig() {
    send("Configure >> BatchSize=10000, FrameSize=" + std::to_string(g_config.frame_size) + ", StorageTarget=Storage -@ Process");
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
}

static void loadConfig() {
    g_config = DaqConfig();
    if (!g_config.loadFromJson(g_initConfigPath)) {
        std::cerr << "[main] Failed to load " << g_initConfigPath << "\n";
        return;
    }
    g_config.slot = 0; // simsrc 无实际硬件，slot 用 0
    std::cout << "[main] Config loaded\n";
}

static void printStatus(const ModuleSim& sim,
                        const ModuleProcess& process,
                        const ModuleStorage& storage) {
    ModuleSim::Stats s = sim.getStats();
    std::cout << "\n======= Frame Summary =======\n";
    std::cout << "  "
              << std::setw(4) << "ch"
              << std::setw(11) << "DAQ"
              << std::setw(11) << "Process"
              << std::setw(11) << "Storage" << "\n";
    for (int i = 0; i < (int)s.total_frames.size(); i++) {
        std::cout << "  "
                  << std::setw(4) << ("ch" + std::to_string(i + 1))
                  << std::setw(11) << s.total_frames[i]
                  << std::setw(11) << process.getProcessedFrames(i)
                  << std::setw(11) << storage.getConsumedFrames(i)
                  << "\n";
    }
    std::cout << "============================\n" << std::endl;
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
        std::string waveData = "1";
        const auto* ch1 = g_config.getChannel(1, 1);
        if (ch1) {
            trigLen = std::stoi(ch1->trig_length);
            waveData = ch1->wave_data_enable;
        }

        msm::Message simCfg("Configure");
        simCfg.setTarget("DAQModule");
        simCfg.setSender("Main");
        simCfg.data.set("NumChannels", nCh);
        simCfg.data.set("TrigLength", trigLen);
        simCfg.data.set("WaveDataEnable", waveData);
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

    UserCircularBuffer saveRing(256 * 1024 * 1024, 4096);

    std::vector<int> physCh;
    for (int i = 0; i < nCh; i++)
        physCh.push_back(i + 1);

    ModuleProcess process("Process", allRings, &saveRing, physCh);
    ModuleStorage storage(&saveRing, physCh);
    bus.registerModule(&process);
    bus.registerModule(&storage);
    process.start();
    storage.start();
    distributeConfig();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "[main] SIM ready: " << sim.getRingCount() << " rings\n";
    std::cout << "[main] Running. Commands: start / stop / enablesave / disablesave / status / reload / quit\n";

    std::thread cli([&]() {
        std::string line;
        while (!g_stop.load() && std::getline(std::cin, line)) {
            if (line.empty()) continue;
            if (line == "quit" || line == "exit") {
                g_stop.store(true);
                break;
            }
            if (line == "start") {
                send("Start >> -@ Storage");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                send("Start >> -@ Process");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                send("Start >> -@ DAQModule");
                continue;
            }
            if (line == "stop") {
                send("Stop >> -@ DAQModule");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                send("Stop >> -@ Process");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                send("Stop >> -@ Storage");
                for (int i = 0; i < 50 && storage.isDraining(); i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                printStatus(sim, process, storage);
                continue;
            }
            if (line == "enablesave") {
                send("EnableSave >> -@ Storage");
                continue;
            }
            if (line == "disablesave") {
                send("DisableSave >> -@ Storage");
                continue;
            }
            if (line == "status") {
                printStatus(sim, process, storage);
                continue;
            }
            if (line == "reload") {
                std::cout << "[main] Reloading...\n";
                send("Stop >> -@ DAQModule");
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                send("Stop >> -@ Process");
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                send("Stop >> -@ Storage");
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
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            send("Stop >> -@ Process");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            send("Stop >> -@ Storage");
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
    send("Stop >> -@ Storage");
    for (int i = 0; i < 50 && storage.isDraining(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    process.stop();
    storage.stop();
    sim.stop();
    ipc.stop();

    std::cin.setstate(std::ios::eofbit);
    close(STDIN_FILENO);

    bus.shutdown();

    if (cli.joinable())
        cli.join();
    return 0;
}
