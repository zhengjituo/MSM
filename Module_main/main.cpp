#include "MessageBus.h"
#include "NIStringParser.h"
#include "IPCServer.h"
#include "daq_module.h"
#include "consumer_module.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <sstream>

static std::atomic<bool> g_stop{false};
static void handle_signal(int) {
    g_stop.store(true);
}

static void send(const std::string& niString) {
    msm::Message msg = msm::NIStringParser::parse(niString, "Main");
    msm::MessageBus::instance().send(msg);
}

static void printStatus(const DAQModule& daq,
                        const ConsumerModule& consumer) {
    DAQModule::Stats s = daq.getStats();
    std::cout << "\n=== Status ===\n";
    std::cout << "  DAQ running=" << s.is_running << "\n";
    for (int i = 0; i < (int)s.total_frames.size(); i++) {
        std::cout << "  ch" << (i + 1)
                  << ": frames=" << s.total_frames[i]
                  << " overflow=" << s.overflow_count[i]
                  << " frame_size=" << s.frame_size[i]
                  << " consumed=" << consumer.getProcessedFrames(i) << "\n";
    }
    std::cout << "================\n" << std::endl;
}

int main() {
    signal(SIGINT, handle_signal);

    auto& bus = msm::MessageBus::instance();

    DAQModule daq;
    daq.loadConfig("config/app_inittial_config.json");

    // 收集 ring buffer 指针传给 ConsumerModule
    std::vector<UserCircularBuffer*> allRings;
    for (int i = 0; i < daq.getRingCount(); i++)
        allRings.push_back(daq.getRing(i));

    ConsumerModule consumer("Consumer", allRings);
    msm::IPCServer ipc("/tmp/msm_daq.sock");

    bus.registerModule(&daq);
    bus.registerModule(&consumer);
    bus.registerModule(&ipc);

    daq.start();
    consumer.start();
    ipc.start();

    std::cout << "[main] Modules started. " << daq.getRingCount()
              << " rings, " << allRings.size() << " consumer threads.\n";
    std::this_thread::sleep_for(std::chrono::seconds(1));
    send("Configure >> -@ DAQModule");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    send("Configure >> BatchSize=10000 -@ Consumer");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    send("Start >> -@ Consumer");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send("Start >> -@ DAQModule");

    std::cout << "[main] Commands sent. Type NI commands or 'quit'/'status':\n";

    std::thread cli([&]() {
        std::string line;
        while (!g_stop.load() && std::getline(std::cin, line)) {
            if (line.empty()) continue;
            if (line == "quit" || line == "exit") {
                g_stop.store(true);
                break;
            }
            if (line == "status") {
                printStatus(daq, consumer);
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
        if (now - lastPrint > std::chrono::seconds(5)) {
            auto s = daq.getStats();
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

    send("Stop >> -@ DAQModule");
    send("Stop >> -@ Consumer");

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    ipc.stop();
    daq.stop();
    consumer.stop();

    printStatus(daq, consumer);
    bus.shutdown();

    if (cli.joinable())
        cli.join();
    return 0;
}
