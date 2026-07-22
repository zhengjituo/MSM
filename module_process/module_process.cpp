#include "module_process.h"
#include "MessageBus.h"
#include "NIStringParser.h"
#include <iostream>
#include <cstring>
#include <cmath>

static inline uint32_t readU32BE(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline uint64_t readU48BE(const uint8_t* p) {
    return ((uint64_t)p[0] << 40) | ((uint64_t)p[1] << 32) |
           ((uint64_t)p[2] << 24) | ((uint64_t)p[3] << 16) |
           ((uint64_t)p[4] << 8)  | (uint64_t)p[5];
}

static inline uint64_t readU64BE(const uint8_t* p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8)  | (uint64_t)p[7];
}

static inline double readDoubleBE(const uint8_t* p) {
    uint64_t val = readU64BE(p);
    double result;
    memcpy(&result, &val, sizeof(result));
    return result;
}

ModuleProcess::ModuleProcess(const std::string& name,
                                     const std::vector<UserCircularBuffer*>& rings,
                                     UserCircularBuffer* saveRing,
                                     const std::vector<int>& physChannels)
    : Module(name), rings_(rings), saveRing_(saveRing)
{
    if (!physChannels.empty())
        physChannels_ = physChannels;
    else
        for (int i = 0; i < (int)rings.size(); i++)
            physChannels_.push_back(i + 1);  // fallback: 1-based index

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
        lastSum_[i].store(0);
        lastX_[i].store(0.0);
        lastY_[i].store(0.0);
        lastI_[i].store(0.0);
        batchData_[i].trig_id.reserve(batchSize_);
        batchData_[i].bunch_id.reserve(batchSize_);
        batchData_[i].sum.reserve(batchSize_);
        batchData_[i].x.reserve(batchSize_);
        batchData_[i].y.reserve(batchSize_);
        batchData_[i].i.reserve(batchSize_);
    }
    idleDelayMs_ = 50;
    batchBuf_.resize(4096);
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

int64_t ModuleProcess::getLastSum(int ch) const {
    if (ch < 0 || ch >= nCh_) return 0;
    return lastSum_[ch].load();
}

double ModuleProcess::getLastX() const {
    return lastX_[0].load();
}

double ModuleProcess::getLastY() const {
    return lastY_[0].load();
}

double ModuleProcess::getLastI() const {
    return lastI_[0].load();
}

ModuleProcess::BatchFrameData ModuleProcess::getLastBatchData(int ch) const {
    if (ch < 0 || ch >= nCh_) return BatchFrameData();
    std::lock_guard<std::mutex> lock(batchMutex_);
    return batchData_[ch];
}

void ModuleProcess::setStorageTarget(const std::string& target) {
    storageTarget_ = target;
}

void ModuleProcess::setFrameIndex(int idx) {
    frameIndex_ = idx;
    std::cout << "[" << name_ << "] FrameIndex set to " << idx << "\n";
}

void ModuleProcess::onIdle() {
    if (consuming_.load() || draining_.load()) {
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
    } else if (msg.cmd == "SetFrameIndex") {
        int idx = 0;
        if (msg.data.contains("index"))
            idx = static_cast<int>(msg.data.getInt("index"));
        setFrameIndex(idx);
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
    draining_ = false;
    for (int i = 0; i < nCh_; i++) {
        lastWritePos_[i].store(rings_[i] ? rings_[i]->getWriteTotal() : 0);
        lastTrigId_[i].store(0);
        lastBunchId_[i].store(0);
        lastSum_[i].store(0);
        lastX_[i].store(0.0);
        lastY_[i].store(0.0);
        lastI_[i].store(0.0);
    }
    consuming_ = true;
    for (int i = 0; i < nCh_; i++) lastPeekedReadPos_[i] = -1;
    std::cout << "[" << name_ << "] Full processing (message-driven, " << nCh_
              << " ch, batch=" << batchSize_ << ")\n";
}

void ModuleProcess::onStop() {
    consuming_ = false;
    draining_ = true;
    std::cout << "[" << name_ << "] Draining remaining frames...\n";
    // keep doConsumeOnce running until ring is empty
    // emergencyStop_ set by doConsumeOnce when drain completes
}

void ModuleProcess::onStopImmediate() {
    consuming_ = false;
    draining_ = false;
    emergencyStop_ = true;
    std::cout << "[" << name_ << "] Stopped";
    for (int i = 0; i < nCh_; i++) {
        std::cout << " ch" << (i + 1) << "=" << processedFrames_[i].load();
        processedFrames_[i].store(0);
        processedBatches_[i].store(0);
    }
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
        response.data.set("ch" + std::to_string(i + 1) + "_sum",
                          std::to_string(lastSum_[i].load()));
    }
    response.data.set("x_pos", std::to_string(lastX_[0].load()));
    response.data.set("y_pos", std::to_string(lastY_[0].load()));
    response.data.set("intensity", std::to_string(lastI_[0].load()));
}

void ModuleProcess::registerVars() {
    for (int i = 0; i < MAX_CH; i++) {
        std::string idx = std::to_string(i + 1);
        declareVar("ch" + idx + "_frames", processedFrames_[i]);
        declareVar("ch" + idx + "_batches", processedBatches_[i]);
        declareVar("ch" + idx + "_trig_id", lastTrigId_[i]);
        declareVar("ch" + idx + "_bunch_id", lastBunchId_[i]);
        declareVar("ch" + idx + "_sum", lastSum_[i]);
        if (i == 0) {
            declareVar("x_pos", lastX_[i]);
            declareVar("y_pos", lastY_[i]);
            declareVar("intensity", lastI_[i]);
        }
    }
}

void ModuleProcess::extractFrame(const uint8_t* frame, size_t flen, int ch, size_t idx) {
    if (frame[0] != 0xEB || frame[1] != 0x91) return;
    if (flen < 72) return;

    uint32_t tid = readU32BE(frame + 17);
    const uint8_t* trailer = frame + flen - 64;
    uint64_t bid = readU48BE(trailer + 2);
    uint32_t sum = readU32BE(trailer + 8);

    batchData_[ch].trig_id[idx] = tid;
    batchData_[ch].bunch_id[idx] = bid;
    batchData_[ch].sum[idx] = sum;

    // X/Y/I only from channel 1
    if (ch == 0) {
        batchData_[ch].x[idx] = readDoubleBE(trailer + 24);
        batchData_[ch].y[idx] = readDoubleBE(trailer + 32);
        batchData_[ch].i[idx] = readDoubleBE(trailer + 40);
    }

    // Update latest values
    lastTrigId_[ch].store((int64_t)tid);
    lastBunchId_[ch].store((int64_t)bid);
    lastSum_[ch].store((int64_t)sum);
    if (ch == 0) {
        lastX_[0].store(batchData_[0].x[idx]);
        lastY_[0].store(batchData_[0].y[idx]);
        lastI_[0].store(batchData_[0].i[idx]);
    }
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
    if (!allReady) {
        if (draining_.load()) {
            bool allEmpty = true;
            for (int i = 0; i < nCh_; i++) {
                if (!rings_[i]) continue;
                size_t avail = rings_[i]->availableFrames();
                if (avail == 0) continue;
                allEmpty = false;

                size_t flen = rings_[i]->getExpectedPacketSize();
                size_t readBytes = avail * flen;
                if (batchBuf_.size() < readBytes)
                    batchBuf_.resize(readBytes);

                size_t got = rings_[i]->readData(batchBuf_.data(), readBytes);
                if (got == 0) continue;

                if (saveRing_) saveRing_->writeFromDevice(batchBuf_.data(), got);

                size_t actualFrames = got / flen;
                processedFrames_[i].fetch_add(actualFrames);
                uint64_t lastWrite = lastWritePos_[i].load();
                lastWritePos_[i].store(lastWrite + got);

                int chPhys = (i < (int)physChannels_.size()) ? physChannels_[i] : (i + 1);
                std::cout << "[" << name_ << "] ch" << chPhys
                          << " drain=" << actualFrames << "\n";
            }
            if (allEmpty) {
                draining_ = false;
                emergencyStop_ = true;
                std::cout << "[" << name_ << "] Drain complete, ready for start\n";
            }
        }
        return;
    }

    int64_t curBatch = processedBatches_[0].load() + 1;

    // Check saveRing space before reading: need room for all channels
    size_t totalNeeded = 0;
    for (int i = 0; i < nCh_; i++) {
        size_t flen = rings_[i]->getExpectedPacketSize();
        totalNeeded += batchSize_ * flen;
    }
    if (saveRing_ && saveRing_->availableSpace() < totalNeeded) {
        std::cout << "[" << name_ << "] saveRing full, skip cycle\n";
        return;
    }

    for (int i = 0; i < nCh_; i++) {
        size_t flen = rings_[i]->getExpectedPacketSize();
        size_t batchBytes = batchSize_ * flen;
        if (batchBuf_.size() < batchBytes)
            batchBuf_.resize(batchBytes);

        size_t got = rings_[i]->readData(batchBuf_.data(), batchBytes);
        if (got < batchBytes) {
            std::cerr << "[" << name_ << "] ch" << (i + 1)
                      << " read " << got << " < " << batchBytes << "\n";
            continue;
        }

        // 转发原始数据到 storage_in_buffer（单 ring，多通道按 ch1..chN 顺序写入）
        if (saveRing_) saveRing_->writeFromDevice(batchBuf_.data(), got);

        // 提取用户所选 index 的帧
        int selIdx = frameIndex_.load();
        if (selIdx >= 0 && (size_t)selIdx < batchSize_) {
            const uint8_t* frame = batchBuf_.data() + (size_t)selIdx * flen;
            extractFrame(frame, flen, i, 0);
            batchData_[i].count = 1;
            batchData_[i].batchNo = curBatch;
        }

        size_t cnt = 0;
        uint32_t firstTrigId = 0;
        uint64_t firstBunchId = 0;
        const uint8_t* firstFrame = batchBuf_.data();
        if (firstFrame[0] == 0xEB && firstFrame[1] == 0x91 && flen >= 72) {
            firstTrigId = readU32BE(firstFrame + 17);
            const uint8_t* trailer = firstFrame + flen - 64;
            firstBunchId = readU48BE(trailer + 2);
            cnt = batchSize_;
        }

        int chPhys = (i < (int)physChannels_.size()) ? physChannels_[i] : (i + 1);
        std::cout << "[" << name_ << "] ch" << chPhys
                  << " batch=" << curBatch
                  << " trig_id=" << firstTrigId
                  << " bunch_id=" << firstBunchId
                  << " cnt=" << cnt
                  << "\n";
    }

    for (int i = 0; i < nCh_; i++) {
        size_t flen = rings_[i]->getExpectedPacketSize();
        uint64_t lastWrite = lastWritePos_[i].load();
        uint64_t needed = (uint64_t)batchSize_ * (uint64_t)flen;
        processedFrames_[i].fetch_add(batchSize_);
        processedBatches_[i].fetch_add(1);
        lastWritePos_[i].store(lastWrite + needed);
    }

    // 通知 WebUI 新 batch 就绪
    {
        std::string body = "BatchReady >> batchNo=" + std::to_string(curBatch)
                         + ", nCh=" + std::to_string(nCh_)
                         + " -@ WebUI";
        msm::Message readyMsg = msm::NIStringParser::parse(body, name_);
        msm::MessageBus::instance().send(readyMsg);
    }
}
