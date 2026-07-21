#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <nlohmann/json.hpp>
#include "Module.h"
#include "MessageBus.h"
#include "MSMGateway.h"
#include "DataChannel.h"
#include "DCProducerModule.h"

using namespace msm;

int main(int argc, char* argv[]) {
    std::string configPath = argc > 1 ? argv[1] : "dc_alpha.json";

    std::ifstream f(configPath);
    if (!f.is_open()) {
        std::cerr << "Cannot open " << configPath << "\n";
        return 1;
    }
    nlohmann::json j;
    f >> j;

    auto prod = j["dc_producer"];
    int dataPort = prod.value("data_port", 9100);
    int framesPerBatch = prod.value("frames_per_batch", 1000);
    int frameSize = prod.value("frame_size", 128);
    long totalBatches = prod.value("total_batches", 2000L);

    // Start data channel (blocks on TCP accept)
    DataChannel::Config dcCfg;
    dcCfg.port = dataPort;
    dcCfg.timeoutMs = 30000;
    DataChannel dc(DataChannel::PRODUCER, dcCfg);
    std::cout << "[Alpha] Waiting for consumer TCP connect on port " << dataPort << "...\n";
    if (!dc.start()) {
        std::cerr << "[Alpha] DataChannel start failed\n";
        return 1;
    }
    std::cout << "[Alpha] Consumer connected (TCP)\n";

    // Set up MSM infrastructure
    DCProducerModule producer("DCProducer", "Beta", "DCConsumer",
                              framesPerBatch, frameSize, totalBatches, &dc);

    MSMGateway gw;
    if (!gw.loadConfig(configPath)) {
        std::cerr << "[Alpha] Failed to load gateway config\n";
        return 1;
    }

    // Register modules
    MessageBus::instance().registerModule(&gw);
    MessageBus::instance().registerModule(&producer);

    // Start
    gw.start();
    producer.start();

    std::cout << "[Alpha] Sending START to producer...\n";
    producer.post(Message{"START"});

    // Wait for completion
    while (!producer.isDone()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "[Alpha] Test complete, waiting for broadcast...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    producer.stop();
    gw.stop();
    MessageBus::instance().unregisterModule("DCProducer");
    MessageBus::instance().unregisterModule("MSMGateway");
    return 0;
}
