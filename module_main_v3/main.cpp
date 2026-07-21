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
#include <fstream>

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
    msm::Message daqMsg("Configure");
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
                  << " bunch_id=" << process.getLastBunchId(i) << "\n";
    }
    std::cout << "================\n" << std::endl;
}

static void writeLiveJSON(const ModuleDAQ& daq,
                          const ModuleProcess& process,
                          const ModuleStorage& storage) {
    ModuleDAQ::Stats dstat = daq.getStats();
    std::string out = "{\n";
    out += "  \"nch\": " + std::to_string(dstat.total_frames.size()) + ",\n";
    out += "  \"running\": " + std::string(dstat.is_running ? "true" : "false") + ",\n";
    out += "  \"channels\": [\n";
    for (size_t i = 0; i < dstat.total_frames.size(); i++) {
        if (i > 0) out += ",\n";
        out += "    {\n";
        out += "      \"ch\": " + std::to_string((int)(i + 1)) + ",\n";
        out += "      \"frames\": " + std::to_string(dstat.total_frames[i]) + ",\n";
        out += "      \"overflow\": " + std::to_string(dstat.overflow_count[i]) + ",\n";
        out += "      \"frame_size\": " + std::to_string(dstat.frame_size[i]) + ",\n";
        out += "      \"processed\": " + std::to_string(process.getProcessedFrames((int)i)) + ",\n";
        out += "      \"batches\": " + std::to_string(process.getProcessedBatches((int)i)) + ",\n";
        out += "      \"trig_id\": " + std::to_string(process.getLastTrigId((int)i)) + ",\n";
        out += "      \"bunch_id\": " + std::to_string(process.getLastBunchId((int)i)) + ",\n";
        out += "      \"saved\": " + std::to_string(storage.getSavedFrames((int)i)) + "\n";
        out += "    }";
    }
    out += "\n  ]\n}\n";
    std::ofstream f("/tmp/msm_live.json");
    f << out;
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

    int tick = 0;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (++tick % 5 == 0)
            writeLiveJSON(daq, process, storage);
    }

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
