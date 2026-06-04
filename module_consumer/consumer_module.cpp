#include "consumer_module.h"
#include "MessageBus.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>

ConsumerModule::ConsumerModule(const std::string& name, UserCircularBuffer* ring)
    : Module(name), ring_(ring)
{
    if (!ring_)
        std::cerr << "[" << name_ << "] WARN: null ring buffer\n";
    vars_.parent = this;
}

ConsumerModule::~ConsumerModule() {
    stop();
}

msm::ModuleVars* ConsumerModule::getVars() {
    return &vars_;
}

int64_t ConsumerModule::getProcessedFrames() const {
    return processedFrames_.load();
}

int64_t ConsumerModule::getProcessedBatches() const {
    return processedBatches_.load();
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
    if (!ring_) {
        std::cerr << "[" << name_ << "] No ring buffer to consume\n";
        return;
    }
    if (running_.load()) {
        std::cerr << "[" << name_ << "] Already running\n";
        return;
    }
    running_ = true;
    consumerThread_ = std::thread(&ConsumerModule::consumerLoop, this);
    std::cout << "[" << name_ << "] Consumer started\n";
}

void ConsumerModule::onStop() {
    if (!running_.load()) return;
    running_ = false;
    if (consumerThread_.joinable())
        consumerThread_.join();
    std::cout << "[" << name_ << "] Consumer stopped, frames="
              << processedFrames_.load() << std::endl;
}

void ConsumerModule::onStatus(msm::Message& response) {
    response.data.set("processed_frames", std::to_string(processedFrames_.load()));
    response.data.set("processed_batches", std::to_string(processedBatches_.load()));
    response.data.set("running", running_.load() ? "true" : "false");
    if (ring_) {
        response.data.set("ring_frames", std::to_string(ring_->availableFrames()));
        response.data.set("overflow", std::to_string(ring_->getOverflowCount()));
    }
}

void ConsumerModule::consumerLoop() {
    size_t batch = batchSize_;

    while (running_.load()) {
        while (running_.load()) {
            if (ring_->availableFrames() >= batch) break;
            std::this_thread::yield();
        }
        if (!running_.load()) break;

        size_t flen = ring_->getExpectedPacketSize();
        size_t needed = batch * flen;
        std::vector<uint8_t> buf(needed);
        size_t got = ring_->readData(buf.data(), needed);
        if (got != needed) continue;

        if (buf[0] != 0xEB || buf[1] != 0x91) continue;

        onBatch(buf.data(), batch, flen);
        processedFrames_.fetch_add(batch);
        processedBatches_.fetch_add(1);
    }
}

void ConsumerModule::onBatch(const uint8_t* data, size_t frameCount, size_t frameSize) {
    uint32_t tid = ((uint32_t)data[17] << 24) | ((uint32_t)data[18] << 16) |
                   ((uint32_t)data[19] << 8)  | (uint32_t)data[20];
    uint64_t bid = 0;
    if (frameSize >= 72) {
        const uint8_t* t = data + frameSize - 64;
        bid = ((uint64_t)t[2] << 40) | ((uint64_t)t[3] << 32) |
              ((uint64_t)t[4] << 24) | ((uint64_t)t[5] << 16) |
              ((uint64_t)t[6] << 8)  | (uint64_t)t[7];
    }

    std::cout << "[" << name_ << "] batch=" << processedBatches_.load() + 1
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
