#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <nlohmann/json.hpp>
#include "Module.h"
#include "MessageBus.h"
#include "MSMGateway.h"
#include "DataChannel.h"
#include "DCConsumerModule.h"

using namespace msm;

int main(int argc, char* argv[]) {
    std::string configPath = argc > 1 ? argv[1] : "dc_beta.json";

    std::ifstream f(configPath);
    if (!f.is_open()) {
        std::cerr << "Cannot open " << configPath << "\n";
        return 1;
    }
    nlohmann::json j;
    f >> j;

    auto cons = j["dc_consumer"];
    std::string dataHost = cons.value("data_host", "127.0.0.1");
    int dataPort = cons.value("data_port", 9100);

    // Connect data channel (connects to Alpha's TCP listener)
    DataChannel::Config dcCfg;
    dcCfg.host = dataHost;
    dcCfg.port = dataPort;
    dcCfg.timeoutMs = 30000;
    DataChannel dc(DataChannel::CONSUMER, dcCfg);
    std::cout << "[Beta] Connecting TCP to " << dataHost << ":" << dataPort << "...\n";
    if (!dc.start()) {
        std::cerr << "[Beta] DataChannel connect failed (is Alpha running?)\n";
        return 1;
    }
    std::cout << "[Beta] TCP connected\n";

    // Set up MSM infrastructure
    DCConsumerModule consumer("DCConsumer", &dc);

    MSMGateway gw;
    if (!gw.loadConfig(configPath)) {
        std::cerr << "[Beta] Failed to load gateway config\n";
        return 1;
    }

    MessageBus::instance().registerModule(&gw);
    MessageBus::instance().registerModule(&consumer);

    gw.start();
    consumer.start();

    std::cout << "[Beta] Waiting for DATA_READY messages...\n";

    // Wait for DONE
    while (!consumer.isDone()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Wait for broadcast to arrive
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    std::cout << "[Beta] Test complete, shutting down...\n";
    consumer.stop();
    gw.stop();
    MessageBus::instance().unregisterModule("DCConsumer");
    MessageBus::instance().unregisterModule("MSMGateway");
    return 0;
}
