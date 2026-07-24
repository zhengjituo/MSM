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
#include <iomanip>
#include <poll.h>
#include <unistd.h>

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

static void distributeConfig(bool isInit = true) {
    msm::Message daqMsg(isInit ? "Init" : "Configure");
    daqMsg.data.data = g_config.toKeyValues();
    daqMsg.setTarget("DAQModule");
    daqMsg.setSender("Main");
    msm::MessageBus::instance().send(daqMsg);

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

static void printStatus(const ModuleDAQ& daq,
                        const ModuleProcess& process,
                        const ModuleStorage& storage) {
    ModuleDAQ::Stats s = daq.getStats();
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

    {
        std::string cfgPath = std::string(MSM_PROJECT_ROOT) + "/config/app_initial_config.json";
        if (!g_config.loadFromJson(cfgPath)) {
            std::cerr << "[main] Failed to load " << cfgPath << "\n";
            return 1;
        }
    }

    int nCh = 0;
    for (const auto& card : g_config.cards)
        for (const auto& ch : card.channels)
            if (ch.runEnable) nCh++;
    if (nCh == 0) nCh = 4;

    size_t frameSize = g_config.frame_size;
    if (frameSize == 0) frameSize = 1152;
    std::vector<UserCircularBuffer*> daqRings;
    for (int i = 0; i < nCh; i++)
        daqRings.push_back(new UserCircularBuffer(256U * 1024U * 1024U, frameSize));

    ModuleDAQ daq;
    daq.setRings(daqRings);
    bus.registerModule(&daq);
    daq.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto controller = std::make_shared<ControllerModule>();
    bus.registerModule(controller.get());
    controller->start();

    msm::IPCServer ipc("/tmp/msm_daq.sock");
    bus.registerModule(&ipc);
    ipc.start();

    {
        msm::Message daqMsg("Init");
        daqMsg.data.data = g_config.toKeyValues();
        daqMsg.setTarget("DAQModule");
        daqMsg.setSender("Main");
        msm::MessageBus::instance().send(daqMsg);
    }
    while (!daq.isConfigured())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::vector<UserCircularBuffer*> allRings = daqRings;

    UserCircularBuffer saveRing(256 * 1024 * 1024, 4096);

    std::vector<int> physCh;
    for (int i = 0; i < daq.getRingCount(); i++)
        physCh.push_back(i + 1);

    ModuleProcess process("Process", allRings, &saveRing, physCh);
    ModuleStorage storage(&saveRing, physCh);
    bus.registerModule(&process);
    bus.registerModule(&storage);
    process.start();
    storage.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Configure Process/Storage (must be after register)
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

    while (!daq.isHwReady())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "[main] DAQ ready: " << daq.getRingCount() << " rings\n";

    std::cout << "[main] Running. IPC: /tmp/msm_daq.sock\n"
              << "Commands: start / stop / enablesave / disablesave / status / reload / quit\n";

    std::thread cli([&]() {
        std::string rbuf;
        while (!g_stop.load()) {
            struct pollfd pfd;
            pfd.fd = STDIN_FILENO;
            pfd.events = POLLIN;
            int rc = poll(&pfd, 1, 200);
            if (rc < 0) { if (errno == EINTR) continue; break; }
            if (rc == 0) continue;  // timeout, re-check g_stop
            if (!(pfd.revents & POLLIN)) continue;
            char buf[4096];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;  // EOF
            for (ssize_t i = 0; i < n; i++) {
                char c = buf[i];
                if (c == '\n') {
                    std::string line = rbuf;
                    rbuf.clear();
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
                        for (int i = 0; i < 50 && storage.isFlushing(); i++)
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
                        printStatus(daq, process, storage);
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
                        g_config = DaqConfig();
                        std::string cfgPath = std::string(MSM_PROJECT_ROOT) + "/config/app_initial_config.json";
                        if (!g_config.loadFromJson(cfgPath)) {
                            std::cerr << "[main] Failed to reload " << cfgPath << "\n";
                            continue;
                        }
                        distributeConfig(false);
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        std::cout << "[main] Reload done. Type 'start' to begin acquisition.\n";
                        continue;
                    }
                    std::cout << "[main] Sending: " << line << std::endl;
                    send(line);
                } else if (c != '\r') {
                    rbuf += c;
                }
            }
        }
    });

    while (!g_stop.load()) {
        if (g_reload.exchange(false)) {
            std::cout << "[main] Reloading (via IPC)...\n";
            send("Stop >> -@ DAQModule");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            send("Stop >> -@ Process");
            send("Stop >> -@ Storage");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            distributeConfig(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            send("Start >> -@ DAQModule");
            std::cout << "[main] Reload done\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\n[main] Shutting down...\n";
    send("Stop >> -@ DAQModule");
    send("Stop >> -@ Process");
    send("Stop >> -@ Storage");
    for (int i = 0; i < 50 && storage.isFlushing(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    process.stop();
    storage.stop();
    daq.stop();
    ipc.stop();

    std::cin.setstate(std::ios::eofbit);
    close(STDIN_FILENO);

    std::thread watchdog([] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::cerr << "[main] FORCE EXIT after 5s timeout\n";
        _exit(1);
    });
    watchdog.detach();

    bus.shutdown();

    if (cli.joinable())
        cli.join();
    std::cout << "[main] exited cleanly.\n";
    return 0;
}
