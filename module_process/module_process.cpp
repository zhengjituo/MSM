#include "module_process.h"
#include "MessageBus.h"
#include <iostream>
#include <cstring>
#include <thread>

ModuleProcess::ModuleProcess(const std::string& name,
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
        lastTrigId_[i].store(0);
        lastBunchId_[i].store(0);
    }
    idleDelayMs_ = 10;
    peekBuf_.resize(4096);
    registerVars();
}

ModuleProcess::~ModuleProcess() {
    stop();
}

msm::ModuleVars* ModuleProcess::getVars() {
    return &tableVars_;
}

int64_t ModuleProcess::getProcessedFrames(int ch) const {
    if (ch < 0 || ch >= nCh_) return 0;
    return processedFrames_[ch].load();
}

int64_t ModuleProcess::getProcessedBatches(int ch) const {
    if (ch < 0 || ch >= nCh_) return 0;
    return processedBatches_[ch].load();
}

int64_t ModuleProcess::getLastTrigId(int ch) const {
    if (ch < 0 || ch >= nCh_) return 0;
    return lastTrigId_[ch].load();
}

int64_t ModuleProcess::getLastBunchId(int ch) const {
    if (ch < 0 || ch >= nCh_) return 0;
    return lastBunchId_[ch].load();
}

void ModuleProcess::setStorageTarget(const std::string& target) {
    storageTarget_ = target;
}

void ModuleProcess::onIdle() {
    if (consuming_.load()) {
        msm::Message consumeOnce("ConsumeOnce");
        post(consumeOnce);
    }
}

void ModuleProcess::handleState(const msm::Message& msg) {
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

void ModuleProcess::onConfigure(const msm::Json& data) {
    if (data.contains("BatchSize"))
        batchSize_ = static_cast<size_t>(data.getInt("BatchSize"));
    if (data.contains("StorageTarget"))
        storageTarget_ = data.get("StorageTarget");
    std::cout << "[" << name_ << "] Configured: BatchSize=" << batchSize_
              << " StorageTarget=" << storageTarget_ << "\n";
}

void ModuleProcess::onStart() {
    emergencyStop_ = false;
    for (int i = 0; i < nCh_; i++) {
        lastWritePos_[i].store(0);
        lastTrigId_[i].store(0);
        lastBunchId_[i].store(0);
    }
    consuming_ = true;
    std::cout << "[" << name_ << "] Processing (message-driven, " << nCh_
              << " ch, batch=" << batchSize_ << ")\n";
}

void ModuleProcess::onStop() {
    consuming_ = false;
    emergencyStop_ = true;
    std::cout << "[" << name_ << "] Stopped";
    for (int i = 0; i < nCh_; i++)
        std::cout << " ch" << (i + 1) << "=" << processedFrames_[i].load();
    std::cout << std::endl;
}

void ModuleProcess::onStatus(msm::Message& response) {
    for (int i = 0; i < nCh_; i++) {
        response.data.set("ch" + std::to_string(i + 1) + "_frames",
                          std::to_string(processedFrames_[i].load()));
        response.data.set("ch" + std::to_string(i + 1) + "_batches",
                          std::to_string(processedBatches_[i].load()));
        response.data.set("ch" + std::to_string(i + 1) + "_trig_id",
                          std::to_string(lastTrigId_[i].load()));
        response.data.set("ch" + std::to_string(i + 1) + "_bunch_id",
                          std::to_string(lastBunchId_[i].load()));
    }
}

void ModuleProcess::registerVars() {
    for (int i = 0; i < MAX_CH; i++) {
        std::string idx = std::to_string(i + 1);
        declareVar("ch" + idx + "_frames", processedFrames_[i]);
        declareVar("ch" + idx + "_batches", processedBatches_[i]);
        declareVar("ch" + idx + "_trig_id", lastTrigId_[i]);
        declareVar("ch" + idx + "_bunch_id", lastBunchId_[i]);
    }
}

static inline uint32_t readU32BE(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline uint64_t readU48BE(const uint8_t* p) {
    return ((uint64_t)p[0] << 40) | ((uint64_t)p[1] << 32) |
           ((uint64_t)p[2] << 24) | ((uint64_t)p[3] << 16) |
           ((uint64_t)p[4] << 8)  | (uint64_t)p[5];
}

void ModuleProcess::doConsumeOnce() {
    if (emergencyStop_.load()) return;

    bool allReady = true;
    for (int i = 0; i < nCh_; i++) {
        if (!rings_[i]) { allReady = false; break; }
        uint64_t writeTotal = rings_[i]->getWriteTotal();
        uint64_t lastWrite = lastWritePos_[i].load();
        uint64_t needed = (uint64_t)batchSize_ * (uint64_t)rings_[i]->getExpectedPacketSize();
        if (writeTotal - lastWrite < needed) { allReady = false; break; }
        if (rings_[i]->availableFrames() < batchSize_) { allReady = false; break; }
    }
    if (!allReady) return;

    for (int i = 0; i < nCh_; i++) {
        size_t flen = rings_[i]->getExpectedPacketSize();
        size_t peeked = rings_[i]->peekData(peekBuf_.data(), flen);
        if (peeked < flen) continue;
        if (peekBuf_[0] != 0xEB || peekBuf_[1] != 0x91) continue;

        uint32_t tid = readU32BE(peekBuf_.data() + 17);
        uint64_t bid = 0;
        if (flen >= 72) {
            bid = readU48BE(peekBuf_.data() + flen - 62);
        }

        lastTrigId_[i].store((int64_t)tid);
        lastBunchId_[i].store((int64_t)bid);

        int64_t batchNo = processedBatches_[i].load() + 1;
        std::cout << "[" << name_ << "] ch" << (i + 1)
                  << " batch=" << batchNo
                  << " frame=" << processedFrames_[i].load()
                  << " TrigID=" << tid
                  << " BunchID=" << bid << "\n";
    }

    msm::Message save("SaveBatchAll");
    save.data.set("nCh", std::to_string(nCh_));
    save.data.set("batchSize", std::to_string(batchSize_));
    save.setTarget(storageTarget_);
    msm::MessageBus::instance().send(save);

    for (int i = 0; i < nCh_; i++) {
        size_t flen = rings_[i]->getExpectedPacketSize();
        uint64_t lastWrite = lastWritePos_[i].load();
        uint64_t needed = (uint64_t)batchSize_ * (uint64_t)flen;
        processedFrames_[i].fetch_add(batchSize_);
        processedBatches_[i].fetch_add(1);
        lastWritePos_[i].store(lastWrite + needed);
    }
}
