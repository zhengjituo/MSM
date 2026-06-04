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

static std::atomic<bool> g_stop{false};
static void handle_signal(int) {
    g_stop.store(true);
}

static void send(const std::string& niString) {
    msm::Message msg = msm::NIStringParser::parse(niString, "Main");
    msm::MessageBus::instance().send(msg);
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

    std::cout << "[main] All modules started, sending Configure + Start\n";

    send("Configure >> TrigLength=1024, VoltageRange=1000, TrigLevel=200 -@ DAQModule");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send("Configure >> BatchSize=10000 -@ Consumer1");
    send("Configure >> BatchSize=10000 -@ Consumer2");
    send("Configure >> BatchSize=10000 -@ Consumer3");
    send("Configure >> BatchSize=10000 -@ Consumer4");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send("Start -@ DAQModule");
    send("Start -@ Consumer1");
    send("Start -@ Consumer2");
    send("Start -@ Consumer3");
    send("Start -@ Consumer4");

    std::cout << "[main] Running. Press Ctrl+C to stop.\n";

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "\n[main] Shutting down...\n";

    send("Stop -@ DAQModule");
    send("Stop -@ Consumer1");
    send("Stop -@ Consumer2");
    send("Stop -@ Consumer3");
    send("Stop -@ Consumer4");

    // Let messages process
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    ipc.stop();
    daq.stop();
    c1.stop();
    c2.stop();
    c3.stop();
    c4.stop();

    DAQModule::Stats s = daq.getStats();
    std::cout << "\n=== Final Stats ===\n";
    for (int i = 0; i < DAQModule::NCH; i++) {
        std::cout << "  ch" << DAQModule::CHANNELS[i]
                  << ": frames=" << s.total_frames[i]
                  << " overflow=" << s.overflow_count[i]
                  << " frame_size=" << s.frame_size[i] << std::endl;
    }
    std::cout << "  Consumer1: " << c1.getProcessedFrames() << " frames, "
              << c1.getProcessedBatches() << " batches\n";
    std::cout << "  Consumer2: " << c2.getProcessedFrames() << " frames, "
              << c2.getProcessedBatches() << " batches\n";
    std::cout << "  Consumer3: " << c3.getProcessedFrames() << " frames, "
              << c3.getProcessedBatches() << " batches\n";
    std::cout << "  Consumer4: " << c4.getProcessedFrames() << " frames, "
              << c4.getProcessedBatches() << " batches\n";

    bus.shutdown();
    return 0;
}
