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

static std::atomic<bool> g_stop{false};
static void handle_signal(int) {
    g_stop.store(true);
}

static DaqConfig g_config;

static void send(const std::string& niString) {
    msm::Message msg = msm::NIStringParser::parse(niString, "Main");
    msm::MessageBus::instance().send(msg);
}

static void distributeConfig() {
    // DAQ: 把 struct 拍平成键值对，塞进 MSG data
    msm::Message daqMsg("Configure");
    daqMsg.data.data = g_config.toKeyValues();
    daqMsg.setTarget("DAQModule");
    daqMsg.setSender("Main");
    msm::MessageBus::instance().send(daqMsg);

    // Process 参数
    send("Configure >> BatchSize=10000, StorageTarget=Storage -@ Process");

    // Storage 参数
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
                  << " saved=" << storage.getSavedFrames(i) << "\n";
    }
    std::cout << "================\n" << std::endl;
}

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    auto& bus = msm::MessageBus::instance();

    // --- 1. 读配置 ---
    if (!g_config.loadFromJson("config/app_initial_config.json")) {
        std::cerr << "[main] Failed to load config\n";
        return 1;
    }

    // --- 2. 创建模块，先启动 DAQ 线程 ---
    ModuleDAQ daq;
    bus.registerModule(&daq);
    daq.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // --- 3. 通过 MSG 下发配置 ---
    distributeConfig();
    while (!daq.isConfigured())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // --- 4. 获取 ring，创建 Process 和 Storage ---
    std::vector<UserCircularBuffer*> allRings;
    for (int i = 0; i < daq.getRingCount(); i++)
        allRings.push_back(daq.getRing(i));

    ModuleProcess process("Process", allRings);
    ModuleStorage storage(allRings);
    msm::IPCServer ipc("/tmp/msm_daq.sock");

    bus.registerModule(&process);
    bus.registerModule(&storage);
    bus.registerModule(&ipc);

    process.start();
    storage.start();
    ipc.start();

    std::cout << "[main] DAQ started: " << daq.getRingCount() << " rings\n";

    // --- 5. 启动采集 ---
    send("Start >> -@ Storage");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send("Start >> -@ Process");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send("Start >> -@ DAQModule");

    std::cout << "[main] Running. Commands: quit/status\n";

    std::thread cli([&]() {
        std::string line;
        while (!g_stop.load() && std::getline(std::cin, line)) {
            if (line.empty()) continue;
            if (line == "quit" || line == "exit") {
                g_stop.store(true);
                break;
            }
            if (line == "status") {
                printStatus(daq, process, storage);
                continue;
            }
            send(line);
        }
    });

    while (!g_stop.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "\n[main] Shutting down...\n";

    send("Stop >> -@ DAQModule");
    send("Stop >> -@ Process");
    send("Stop >> -@ Storage");

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

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
