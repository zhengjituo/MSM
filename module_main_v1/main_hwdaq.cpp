#include "MessageBus.h"
#include "NIStringParser.h"
#include "IPCServer.h"
#include "module_daq.h"
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
    msm::Message daqMsg("Init");
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

static void loadConfig() {
    g_config = DaqConfig();
    if (!g_config.loadFromJson(g_initConfigPath)) {
        std::cerr << "[main] Failed to load " << g_initConfigPath << "\n";
        return;
    }
    std::cout << "[main] Config loaded\n";
}

static void printStatus(const ModuleDAQ& daq,
                        const ModuleProcessFull& process,
                        const ModuleStorage& storage) {
    ModuleDAQ::Stats s = daq.getStats();
    std::cout << "=== Status ==============================\n"
              << "  DAQ: " << (s.is_running ? "采集" : "停止") << "\n";
    for (int i = 0; i < (int)s.total_frames.size(); i++) {
        std::cout << "  ch" << (i + 1)
                  << "  daq_frames=" << s.total_frames[i]
                  << "  proc_frames=" << process.getProcessedFrames(i)
                  << "  proc_batches=" << process.getProcessedBatches(i)
                  << "\n";
    }
    std::cout << "=========================================\n" << std::endl;
}

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    auto& bus = msm::MessageBus::instance();

    loadConfig();

    ModuleDAQ daq;
    bus.registerModule(&daq);
    daq.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::vector<UserCircularBuffer*> allRings;
    // 先创建 process/storage（ring 列表会在 InitHW 后填充）
    // 但 ModuleDAQ 的 ring 在 Init 内部 ReadConfig 阶段分配
    // 所以需要在 distributeConfig 之后收集 ring

    auto controller = std::make_shared<ControllerModule>();
    bus.registerModule(controller.get());
    controller->start();

    msm::IPCServer ipc("/tmp/msm_daq.sock");
    bus.registerModule(&ipc);
    ipc.start();

    // Init: ReadConfig → InitHW → UpdateConfig
    distributeConfig();
    while (!daq.isConfigured())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // 此时 ring 已分配
    for (int i = 0; i < daq.getRingCount(); i++)
        allRings.push_back(daq.getRing(i));

    ModuleProcessFull process("Process", allRings);
    ModuleStorage storage(nullptr, {});
    bus.registerModule(&process);
    bus.registerModule(&storage);
    process.start();
    // Storage 不 start — recording_=false，discard 模式

    while (!daq.isHwReady())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "[main] DAQ ready: " << daq.getRingCount() << " rings\n";

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
                printStatus(daq, process, storage);
                continue;
            }
            if (line == "reload") {
                std::cout << "[main] Reloading...\n";
                send("Stop >> -@ DAQModule");
                send("Stop >> -@ Process");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                loadConfig();
                distributeConfig();  // Init: onReadConfig → updateConfig (跳过 initHW)
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
    send("CloseHW >> -@ DAQModule");

    // 关闭 stdin 释放 CLI 线程的 getline 阻塞
    std::cin.setstate(std::ios::eofbit);
    close(STDIN_FILENO);

    ipc.stop();
    daq.stop();
    process.stop();
    storage.stop();

    printStatus(daq, process, storage);
    bus.shutdown();

    if (cli.joinable())
        cli.join();
    return 0;
}
