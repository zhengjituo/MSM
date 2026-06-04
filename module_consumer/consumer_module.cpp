#include "consumer_module.h"
#include "MessageBus.h"
#include <iostream>
#include <cstring>
#include <thread>

ConsumerModule::ConsumerModule(const std::string& name,
                               const std::vector<UserCircularBuffer*>& rings)
    : Module(name), rings_(rings)
{
    nCh_ = (int)rings_.size();
    if (nCh_ > MAX_CH) {
        std::cerr << "[" << name_ << "] WARN: " << nCh_ << " rings > MAX_CH="
                  << MAX_CH << ", truncating\n";
        nCh_ = MAX_CH;
    }
    for (int i = 0; i < nCh_; i++) {
        if (!rings_[i])
            std::cerr << "[" << name_ << "] WARN: null ring[" << i << "]\n";
    }
    vars_.parent = this;
    idleDelayMs_ = 10;
}

ConsumerModule::~ConsumerModule() {
    stop();
}

msm::ModuleVars* ConsumerModule::getVars() {
    return &vars_;
}

int64_t ConsumerModule::getProcessedFrames(int ch) const {
    if (ch < 0 || ch >= nCh_) return 0;
    return processedFrames_[ch].load();
}

int64_t ConsumerModule::getProcessedBatches(int ch) const {
    if (ch < 0 || ch >= nCh_) return 0;
    return processedBatches_[ch].load();
}

void ConsumerModule::onIdle() {
    if (consuming_.load()) {
        msm::Message consumeOnce("ConsumeOnce");
        post(consumeOnce);
    }
}

void ConsumerModule::handleState(const msm::Message& msg) {
    if (msg.cmd == "ConsumeOnce") {
        if (!emergencyStop_.load())
            doConsumeOnce();

    } else if (msg.cmd == "Configure") {
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
    emergencyStop_ = false;
    consuming_ = true;
    std::cout << "[" << name_ << "] Consuming (message-driven)\n";
}

void ConsumerModule::onStop() {
    consuming_ = false;
    emergencyStop_ = true;
    std::cout << "[" << name_ << "] Stopped";
    for (int i = 0; i < nCh_; i++)
        std::cout << " ch" << (i + 1) << "=" << processedFrames_[i].load();
    std::cout << std::endl;
}

void ConsumerModule::onStatus(msm::Message& response) {
    for (int i = 0; i < nCh_; i++) {
        response.data.set("ch" + std::to_string(i + 1) + "_frames",
                          std::to_string(processedFrames_[i].load()));
        response.data.set("ch" + std::to_string(i + 1) + "_batches",
                          std::to_string(processedBatches_[i].load()));
    }
}

void ConsumerModule::doConsumeOnce() {
    if (emergencyStop_.load()) return;

    bool any = false;
    for (int i = 0; i < nCh_; i++) {
        if (emergencyStop_.load()) return;
        if (!rings_[i]) continue;

        size_t avail = rings_[i]->availableFrames();
        if (avail < batchSize_) continue;

        size_t flen = rings_[i]->getExpectedPacketSize();
        size_t needed = batchSize_ * flen;
        std::vector<uint8_t> buf(needed);
        size_t got = rings_[i]->readData(buf.data(), needed);
        if (got != needed) continue;

        if (buf[0] != 0xEB || buf[1] != 0x91) continue;

        onBatch(i, buf.data(), batchSize_, flen);
        processedFrames_[i].fetch_add(batchSize_);
        processedBatches_[i].fetch_add(1);
        any = true;
    }
    if (any) {
        std::cout << "[" << name_ << "]";
        for (int i = 0; i < nCh_; i++)
            std::cout << " ch" << (i + 1) << "=" << processedFrames_[i].load();
        std::cout << std::endl;
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

    // onBatch print suppressed — summary in doConsumeOnce
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
