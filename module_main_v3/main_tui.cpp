#include "MessageBus.h"
#include "NIStringParser.h"
#include "IPCServer.h"
#include "module_daq.h"
#include "module_process_full.h"
#include "module_storage.h"
#include "daq_config.h"
#include "module_tui.h"
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
        if (msg.cmd == "Reload") g_reload.store(true);
        else if (msg.cmd == "Quit") g_stop.store(true);
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
    if (!g_config.loadFromJson(g_initConfigPath))
        std::cerr << "[main] Failed to load " << g_initConfigPath << "\n";
    else
        std::cout << "[main] Config loaded\n";
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

    auto controller = std::make_shared<ControllerModule>();
    bus.registerModule(controller.get());
    controller->start();

    msm::IPCServer ipc("/tmp/msm_daq.sock");
    bus.registerModule(&ipc);
    ipc.start();

    distributeConfig();
    while (!daq.isConfigured())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    for (int i = 0; i < daq.getRingCount(); i++)
        allRings.push_back(daq.getRing(i));

    // 从 DAQ 获取物理通道号
    auto daqStats = daq.getStats();
    std::vector<int> physChannels = daqStats.channel_ids;
    // 创建 storage_in_buffer（单 ring，Process→Storage，64MB）
    UserCircularBuffer* saveRing = new UserCircularBuffer(64U * 1024U * 1024U, 384);

    ModuleProcessFull process("Process", allRings, saveRing, physChannels);
    ModuleStorage storage(saveRing, physChannels);
    bus.registerModule(&process);
    bus.registerModule(&storage);
    process.start();

    while (!daq.isHwReady())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "[main] DAQ ready: " << daq.getRingCount() << " rings\n";

    // ── TUI setup ──
    TUI tui;
    tui.init();

    tui.run(
        // command callback
        [&](const std::string& cmd) {
        if (cmd == "quit" || cmd == "exit") {
            g_stop.store(true);
            return;
        }
        if (cmd == "start") {
            send("Start >> -@ Process");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            send("Start >> -@ DAQModule");
            return;
        }
        if (cmd == "stop") {
            send("Stop >> -@ DAQModule");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            send("Stop >> -@ Process");
            return;
        }
        if (cmd == "status" || cmd == "st") {
            ModuleDAQ::Stats s = daq.getStats();
            std::cout << "DAQ: " << (s.is_running ? "running" : "stopped") << "\n";
            for (int i = 0; i < (int)s.total_frames.size(); i++)
                std::cout << "  ch" << (i + 1) << " daq=" << s.total_frames[i]
                          << " proc=" << process.getProcessedFrames(i)
                          << " batch=" << process.getProcessedBatches(i) << "\n";
            return;
        }
        if (cmd == "reload") {
            std::cout << "[main] Reloading...\n";
            send("Stop >> -@ Process");
            send("Stop >> -@ DAQModule");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            loadConfig();
            distributeConfig();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            send("Start >> -@ Process");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            send("Start >> -@ DAQModule");
            std::cout << "[main] Reload done\n";
            return;
        }
        std::cout << "[main] Sending: " << cmd << "\n";
        send(cmd);
    },
        // tick callback (called ~10 times/sec)
        [&]() {
            if (g_stop.load()) {
                tui.stop();
                return;
            }
            if (g_reload.exchange(false)) {
                std::cout << "[main] Reloading...\n";
                send("Stop >> -@ Process");
                send("Stop >> -@ DAQModule");
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                loadConfig();
                distributeConfig();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                send("Start >> -@ Process");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                send("Start >> -@ DAQModule");
                std::cout << "[main] Reload done\n";
            }
        });

    if (!g_stop.load()) g_stop.store(true);

    std::cout << "\n[main] Shutting down...\n";

    send("Stop >> -@ DAQModule");
    send("Stop >> -@ Process");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    send("CloseHW >> -@ DAQModule");

    tui.stop();
    ipc.stop();
    daq.stop();
    process.stop();
    storage.stop();

    delete saveRing;

    bus.shutdown();
    return 0;
}
