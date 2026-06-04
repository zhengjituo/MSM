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
                        const ConsumerModule& c1,
                        const ConsumerModule& c2,
                        const ConsumerModule& c3,
                        const ConsumerModule& c4) {
    DAQModule::Stats s = daq.getStats();
    std::cout << "\n=== Status ===\n";
    std::cout << "  DAQ running=" << s.is_running << "\n";
    for (int i = 0; i < DAQModule::NCH; i++) {
        std::cout << "  ch" << DAQModule::CHANNELS[i]
                  << ": frames=" << s.total_frames[i]
                  << " overflow=" << s.overflow_count[i]
                  << " frame_size=" << s.frame_size[i] << "\n";
    }
    std::cout << "  Consumer1: " << c1.getProcessedFrames() << " frames\n";
    std::cout << "  Consumer2: " << c2.getProcessedFrames() << " frames\n";
    std::cout << "  Consumer3: " << c3.getProcessedFrames() << " frames\n";
    std::cout << "  Consumer4: " << c4.getProcessedFrames() << " frames\n";
    std::cout << "================\n" << std::endl;
}

int main() {
    signal(SIGINT, handle_signal);

    auto& bus = msm::MessageBus::instance();

    DAQModule daq;
    ConsumerModule c1("Consumer1", daq.getRing(0));
    ConsumerModule c2("Consumer2", daq.getRing(1));
    ConsumerModule c3("Consumer3", daq.getRing(2));
    ConsumerModule c4("Consumer4", daq.getRing(3));
    msm::IPCServer ipc("/tmp/msm_daq.sock");

    bus.registerModule(&daq);
    bus.registerModule(&c1);
    bus.registerModule(&c2);
    bus.registerModule(&c3);
    bus.registerModule(&c4);
    bus.registerModule(&ipc);

    daq.start();
    c1.start();
    c2.start();
    c3.start();
    c4.start();
    ipc.start();

    std::cout << "[main] Modules started. Sending Configure + Start in 1s...\n";
    std::this_thread::sleep_for(std::chrono::seconds(1));

    send("Configure >> TrigLength=1024, VoltageRange=1000, TrigLevel=200 -@ DAQModule");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    send("Configure >> BatchSize=10000 -@ Consumer1");
    send("Configure >> BatchSize=10000 -@ Consumer2");
    send("Configure >> BatchSize=10000 -@ Consumer3");
    send("Configure >> BatchSize=10000 -@ Consumer4");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    send("Start -@ DAQModule");
    send("Start -@ Consumer1");
    send("Start -@ Consumer2");
    send("Start -@ Consumer3");
    send("Start -@ Consumer4");

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
                printStatus(daq, c1, c2, c3, c4);
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
            for (int i = 0; i < DAQModule::NCH; i++)
                std::cout << " ch" << DAQModule::CHANNELS[i]
                          << "=" << s.total_frames[i];
            std::cout << std::endl;
            lastPrint = now;
        }
    }

    std::cout << "\n[main] Shutting down...\n";

    send("Stop -@ DAQModule");
    send("Stop -@ Consumer1");
    send("Stop -@ Consumer2");
    send("Stop -@ Consumer3");
    send("Stop -@ Consumer4");

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    ipc.stop();
    daq.stop();
    c1.stop();
    c2.stop();
    c3.stop();
    c4.stop();

    printStatus(daq, c1, c2, c3, c4);
    bus.shutdown();

    if (cli.joinable())
        cli.join();
    return 0;
}
