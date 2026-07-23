#include "MessageBus.h"
#include "NIStringParser.h"
#include "IPCServer.h"
#include "module_daq.h"
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
                        const ModuleProcess& process,
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

    ModuleProcess process("Process", allRings);
    ModuleStorage storage(nullptr, {});
    msm::IPCServer ipc("/tmp/msm_daq.sock");

    bus.registerModule(&process);
    bus.registerModule(&storage);
    bus.registerModule(&ipc);

    process.start();
    storage.start();
    ipc.start();

    // Init 帧内部顺序执行: ReadConfig → InitHW → UpdateConfig
    while (!daq.isHwReady())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "[main] DAQ started: " << daq.getRingCount() << " rings\n";
    std::cout << "[main] Storage not started (recording disabled). Use enablesave to enable.\n";
    // Process Start controlled by DAQ::onStart
    send("Start >> -@ DAQModule");

    std::cout << "[main] Running. IPC: /tmp/msm_daq.sock\n";

    int debugCnt = 0;
    while (!g_stop.load()) {
        if (++debugCnt % 20 == 0) {
            auto dstat = daq.getStats();
            int n = std::max((int)dstat.total_frames.size(), daq.getRingCount());
            for (int i = 0; i < n && i < 1; i++) {
                auto* ring = daq.getRing(i);
                if (ring) {
                    uint64_t wt = ring->getWriteTotal();
                    uint64_t rt = ring->getReadTotal();
                    uint64_t cap = ring->getAlignCapacity();
                    size_t af = ring->availableFrames();
                    size_t flen = ring->getExpectedPacketSize();
                    std::cout << "[DEBUG] ch" << (i+1)
                              << " ring: wt=" << wt << " rt=" << rt
                              << " cap=" << cap << " flen=" << flen
                              << " availFrames=" << af
                              << " procFrames=" << process.getProcessedFrames(i)
                              << " procBatches=" << process.getProcessedBatches(i)
                              << " consuming=" << (process.isConsuming() ? 1 : 0)
                              << " acquired=" << (dstat.is_running ? 1 : 0)
                              << "\n";
                }
            }
        }
        if (g_reload.exchange(false)) {
            std::cout << "[main] Reloading config...\n";
            send("Stop >> ->| Storage");
            send("Stop >> ->| DAQModule");  // DAQ internally stops Process
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            distributeConfig();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            send("Start >> -@ DAQModule");  // DAQ internally starts Process
            std::cout << "[main] Reload done\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\n[main] Shutting down...\n";

    send("Stop >> -@ DAQModule");  // DAQ internally stops Process
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
