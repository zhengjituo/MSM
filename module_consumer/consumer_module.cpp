#include "consumer_module.h"
#include "MessageBus.h"
#include <iostream>
#include <cstring>
#include <thread>

ConsumerModule::ConsumerModule(const std::string& name,
                               UserCircularBuffer* ring0,
                               UserCircularBuffer* ring1,
                               UserCircularBuffer* ring2,
                               UserCircularBuffer* ring3)
    : Module(name)
{
    rings_[0] = ring0;
    rings_[1] = ring1;
    rings_[2] = ring2;
    rings_[3] = ring3;
    for (int i = 0; i < NCH; i++)
        if (!rings_[i])
            std::cerr << "[" << name_ << "] WARN: null ring[" << i << "]\n";
    vars_.parent = this;
}

ConsumerModule::~ConsumerModule() {
    stop();
}

msm::ModuleVars* ConsumerModule::getVars() {
    return &vars_;
}

int64_t ConsumerModule::getProcessedFrames(int ch) const {
    if (ch < 0 || ch >= NCH) return 0;
    return processedFrames_[ch].load();
}

int64_t ConsumerModule::getProcessedBatches(int ch) const {
    if (ch < 0 || ch >= NCH) return 0;
    return processedBatches_[ch].load();
}

void ConsumerModule::handleState(const msm::Message& msg) {
    if (msg.cmd == "Configure") {
        onConfigure(msg.data);
    } else if (msg.cmd == "Start") {
        onStart();
    } else if (msg.cmd == "Stop") {
        onStop();
    } else if (msg.cmd == "Status") {
        msm::Message response("Response");
        response.setTarget(msg.getSender());
        response.setCorrelationId(msg.getCorrelationId());
        onStatus(response);
        msm::MessageBus::instance().send(response);
    }
}

void ConsumerModule::onConfigure(const msm::Json& data) {
    if (data.contains("BatchSize"))
        batchSize_ = static_cast<size_t>(data.getInt("BatchSize"));
    std::cout << "[" << name_ << "] Configured: BatchSize=" << batchSize_ << std::endl;
}

void ConsumerModule::onStart() {
    if (running_.load()) return;
    running_ = true;
    for (int i = 0; i < NCH; i++) {
        if (rings_[i])
            consumerThreads_[i] = std::thread(&ConsumerModule::consumerLoop, this, i, rings_[i]);
    }
    std::cout << "[" << name_ << "] " << NCH << " consumer threads started\n";
}

void ConsumerModule::onStop() {
    if (!running_.load()) return;
    running_ = false;
    for (int i = 0; i < NCH; i++) {
        if (consumerThreads_[i].joinable())
            consumerThreads_[i].join();
    }
    std::cout << "[" << name_ << "] Stopped";
    for (int i = 0; i < NCH; i++)
        std::cout << " ch" << (i + 1) << "=" << processedFrames_[i].load();
    std::cout << std::endl;
}

void ConsumerModule::onStatus(msm::Message& response) {
    for (int i = 0; i < NCH; i++) {
        response.data.set("ch" + std::to_string(i + 1) + "_frames",
                          std::to_string(processedFrames_[i].load()));
        response.data.set("ch" + std::to_string(i + 1) + "_batches",
                          std::to_string(processedBatches_[i].load()));
    }
}

void ConsumerModule::consumerLoop(int chIdx, UserCircularBuffer* ring) {
    size_t batch = batchSize_;

    while (running_.load()) {
        while (running_.load()) {
            if (ring->availableFrames() >= batch) break;
            std::this_thread::yield();
        }
        if (!running_.load()) break;

        size_t flen = ring->getExpectedPacketSize();
        size_t needed = batch * flen;
        std::vector<uint8_t> buf(needed);
        size_t got = ring->readData(buf.data(), needed);
        if (got != needed) continue;

        if (buf[0] != 0xEB || buf[1] != 0x91) continue;

        onBatch(chIdx, buf.data(), batch, flen);
        processedFrames_[chIdx].fetch_add(batch);
        processedBatches_[chIdx].fetch_add(1);
    }
}

void ConsumerModule::onBatch(int channel, const uint8_t* data,
                             size_t frameCount, size_t frameSize) {
    uint32_t tid = ((uint32_t)data[17] << 24) | ((uint32_t)data[18] << 16) |
                   ((uint32_t)data[19] << 8)  | (uint32_t)data[20];
    uint64_t bid = 0;
    if (frameSize >= 72) {
        const uint8_t* t = data + frameSize - 64;
        bid = ((uint64_t)t[2] << 40) | ((uint64_t)t[3] << 32) |
              ((uint64_t)t[4] << 24) | ((uint64_t)t[5] << 16) |
              ((uint64_t)t[6] << 8)  | (uint64_t)t[7];
    }

    std::cout << "[" << name_ << "] ch" << (channel + 1)
              << " batch=" << processedBatches_[channel].load() + 1
              << " frames=" << frameCount
              << " TrigID=" << tid
              << " BunchID=" << bid << std::endl;
}

// ── ConsumerVars ────────────────────────────────────────────────

void* ConsumerModule::ConsumerVars::get(const std::string& name) {
    return nullptr;
}

bool ConsumerModule::ConsumerVars::set(const std::string& name, const std::string& value) {
    return false;
}

std::map<std::string, std::string> ConsumerModule::ConsumerVars::list() {
    return {};
}
