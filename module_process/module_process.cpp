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
    }
    vars_.parent = this;
    idleDelayMs_ = 10;
    peekBuf_.resize(4096);
}

ModuleProcess::~ModuleProcess() {
    stop();
}

msm::ModuleVars* ModuleProcess::getVars() {
    return &vars_;
}

int64_t ModuleProcess::getProcessedFrames(int ch) const {
    if (ch < 0 || ch >= nCh_) return 0;
    return processedFrames_[ch].load();
}

int64_t ModuleProcess::getProcessedBatches(int ch) const {
    if (ch < 0 || ch >= nCh_) return 0;
    return processedBatches_[ch].load();
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
    for (int i = 0; i < nCh_; i++)
        lastWritePos_[i].store(0);
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
    }
}

void ModuleProcess::doConsumeOnce() {
    if (emergencyStop_.load()) return;

    // 检查所有通道是否有足够数据
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

    // peek 每通道首帧，打印摘要
    for (int i = 0; i < nCh_; i++) {
        size_t flen = rings_[i]->getExpectedPacketSize();
        size_t peeked = rings_[i]->peekData(peekBuf_.data(), flen);
        if (peeked < flen) continue;
        if (peekBuf_[0] != 0xEB || peekBuf_[1] != 0x91) continue;

        uint32_t tid = ((uint32_t)peekBuf_[17] << 24) |
                       ((uint32_t)peekBuf_[18] << 16) |
                       ((uint32_t)peekBuf_[19] << 8)  | (uint32_t)peekBuf_[20];
        uint64_t bid = 0;
        if (flen >= 72) {
            const uint8_t* t = peekBuf_.data() + flen - 64;
            bid = ((uint64_t)t[2] << 40) | ((uint64_t)t[3] << 32) |
                  ((uint64_t)t[4] << 24) | ((uint64_t)t[5] << 16) |
                  ((uint64_t)t[6] << 8)  | (uint64_t)t[7];
        }

        int64_t batchNo = processedBatches_[i].load() + 1;
        std::cout << "[" << name_ << "] ch" << (i + 1)
                  << " batch=" << batchNo
                  << " frame=" << processedFrames_[i].load()
                  << " TrigID=" << tid
                  << " BunchID=" << bid << "\n";
    }

    // 发 1 条合并消息，Storage 不用再检查
    msm::Message save("SaveBatchAll");
    save.data.set("nCh", std::to_string(nCh_));
    save.data.set("batchSize", std::to_string(batchSize_));
    save.setTarget(storageTarget_);
    msm::MessageBus::instance().send(save);

    // 更新统计
    for (int i = 0; i < nCh_; i++) {
        size_t flen = rings_[i]->getExpectedPacketSize();
        uint64_t lastWrite = lastWritePos_[i].load();
        uint64_t needed = (uint64_t)batchSize_ * (uint64_t)flen;
        processedFrames_[i].fetch_add(batchSize_);
        processedBatches_[i].fetch_add(1);
        lastWritePos_[i].store(lastWrite + needed);
    }
}

// ── ProcessVars ────────────────────────────────────────────────

void* ModuleProcess::ProcessVars::get(const std::string& name) {
    return nullptr;
}

bool ModuleProcess::ProcessVars::set(const std::string& name, const std::string& value) {
    return false;
}

std::map<std::string, std::string> ModuleProcess::ProcessVars::list() {
    return {};
}
