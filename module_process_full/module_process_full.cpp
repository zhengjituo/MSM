#include "module_process_full.h"
#include "MessageBus.h"
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

ModuleProcessFull::ModuleProcessFull(const std::string& name,
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
    idleDelayMs_ = 10;
    batchBuf_.resize(4096);
    registerVars();
}

ModuleProcessFull::~ModuleProcessFull() {
    stop();
}

msm::ModuleVars* ModuleProcessFull::getVars() {
    return &tableVars_;
}

int64_t ModuleProcessFull::getProcessedFrames(int ch) const {
    if (ch < 0 || ch >= nCh_) return 0;
    return processedFrames_[ch].load();
}

int64_t ModuleProcessFull::getProcessedBatches(int ch) const {
    if (ch < 0 || ch >= nCh_) return 0;
    return processedBatches_[ch].load();
}

int64_t ModuleProcessFull::getLastTrigId(int ch) const {
    if (ch < 0 || ch >= nCh_) return 0;
    return lastTrigId_[ch].load();
}

int64_t ModuleProcessFull::getLastBunchId(int ch) const {
    if (ch < 0 || ch >= nCh_) return 0;
    return lastBunchId_[ch].load();
}

int64_t ModuleProcessFull::getLastSum(int ch) const {
    if (ch < 0 || ch >= nCh_) return 0;
    return lastSum_[ch].load();
}

double ModuleProcessFull::getLastX() const {
    return lastX_[0].load();
}

double ModuleProcessFull::getLastY() const {
    return lastY_[0].load();
}

double ModuleProcessFull::getLastI() const {
    return lastI_[0].load();
}

ModuleProcessFull::BatchFrameData ModuleProcessFull::getLastBatchData(int ch) const {
    if (ch < 0 || ch >= nCh_) return BatchFrameData();
    std::lock_guard<std::mutex> lock(batchMutex_);
    return batchData_[ch];
}

void ModuleProcessFull::setStorageTarget(const std::string& target) {
    storageTarget_ = target;
}

void ModuleProcessFull::onIdle() {
    if (consuming_.load()) {
        msm::Message consumeOnce("ConsumeOnce");
        post(consumeOnce);
    }
}

void ModuleProcessFull::handleState(const msm::Message& msg) {
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

void ModuleProcessFull::onConfigure(const msm::Json& data) {
    if (data.contains("BatchSize"))
        batchSize_ = static_cast<size_t>(data.getInt("BatchSize"));
    if (data.contains("StorageTarget"))
        storageTarget_ = data.get("StorageTarget");
    std::cout << "[" << name_ << "] Configured: BatchSize=" << batchSize_
              << " StorageTarget=" << storageTarget_ << "\n";
}

void ModuleProcessFull::onStart() {
    emergencyStop_ = false;
    for (int i = 0; i < nCh_; i++) {
        lastWritePos_[i].store(rings_[i] ? (int64_t)rings_[i]->getReadTotal() : 0);
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

void ModuleProcessFull::onStop() {
    consuming_ = false;
    emergencyStop_ = true;
    std::cout << "[" << name_ << "] Stopped";
    for (int i = 0; i < nCh_; i++)
        std::cout << " ch" << (i + 1) << "=" << processedFrames_[i].load();
    std::cout << std::endl;
}

void ModuleProcessFull::onStatus(msm::Message& response) {
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

void ModuleProcessFull::registerVars() {
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

void ModuleProcessFull::extractFrame(const uint8_t* frame, size_t flen, int ch, size_t idx) {
    if (frame[0] != 0xEB || frame[1] != 0x91) return;
    if (flen < 72) return;

    uint32_t tid = readU32BE(frame + 17);
    const uint8_t* trailer = frame + flen - 64;
    if (idx == 0) { std::cerr << "[HEX] ch" << (ch+1) << " trailer[0..7]: "; for (int _b=0; _b<8; _b++) std::cerr << std::hex << (int)((const uint8_t*)trailer)[_b] << " "; std::cerr << std::dec << "\n"; }
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

void ModuleProcessFull::doConsumeOnce() {
    if (emergencyStop_.load()) return;

    bool allReady = true;
    for (int i = 0; i < nCh_; i++) {
        if (!rings_[i]) { allReady = false; break; }
        uint64_t writeTotal = rings_[i]->getWriteTotal();
        uint64_t lastWrite = lastWritePos_[i].load();
        uint64_t needed = (uint64_t)batchSize_ * (uint64_t)rings_[i]->getExpectedPacketSize();
        if (writeTotal - lastWrite < needed) { allReady = false; break; }
        if (rings_[i]->availableFrames() < batchSize_) { allReady = false; break; }
        int64_t curRead = (int64_t)rings_[i]->getReadTotal();
        if (curRead <= lastPeekedReadPos_[i]) { allReady = false; break; }

    }
    if (!allReady) return;

    int64_t curBatch = processedBatches_[0].load() + 1;

    for (int i = 0; i < nCh_; i++) {
        size_t flen = rings_[i]->getExpectedPacketSize();
        size_t batchBytes = batchSize_ * flen;
        if (batchBuf_.size() < batchBytes)
            batchBuf_.resize(batchBytes);

        size_t peeked = rings_[i]->peekData(batchBuf_.data(), batchBytes);
        if (peeked < batchBytes) {
            std::cerr << "[" << name_ << "] ch" << (i + 1)
                      << " peeked " << peeked << " < " << batchBytes << "\n";
            continue;
        }

        // Allocate batch data storage
        {
            std::lock_guard<std::mutex> lock(batchMutex_);
            batchData_[i].trig_id.assign(batchSize_, 0);
            batchData_[i].bunch_id.assign(batchSize_, 0);
            batchData_[i].sum.assign(batchSize_, 0);
            batchData_[i].x.assign(batchSize_, 0.0);
            batchData_[i].y.assign(batchSize_, 0.0);
            batchData_[i].i.assign(batchSize_, 0.0);
            batchData_[i].batchNo = curBatch;
            batchData_[i].count = 0;
        }

        // Extract every frame
        {
            std::lock_guard<std::mutex> lock(batchMutex_);
            for (size_t f = 0; f < batchSize_; f++) {
                const uint8_t* frame = batchBuf_.data() + f * flen;
                if (frame[0] != 0xEB || frame[1] != 0x91) continue;
                extractFrame(frame, flen, i, f);
                batchData_[i].count++;
            }

            std::cout << "[" << name_ << "] ch" << (i + 1)
                      << " batch=" << curBatch
                      << " t0=" << batchData_[i].trig_id[0]
                      << " t1=" << batchData_[i].trig_id[1]
                      << " t2=" << batchData_[i].trig_id[2]
                      << " b0=" << batchData_[i].bunch_id[0]
                      << " b1=" << batchData_[i].bunch_id[1]
                      << " cnt=" << batchData_[i].count
                      << "\n";
        }
    }

    // Cross-channel alignment check: all channels must have same trig_id[0]
    {
        int64_t refTrig = batchData_[0].trig_id[0];
        bool aligned = true;
        for (int i = 1; i < nCh_; i++) {
            if (batchData_[i].trig_id[0] != refTrig) {
                std::cerr << "[" << name_ << "] MISALIGN ch" << (i+1) << " trig[0]="
                          << batchData_[i].trig_id[0] << " != ch1=" << refTrig << ", discarding batch\n";
                aligned = false;
            }
        }
        if (!aligned) {
            for (int i = 0; i < nCh_; i++) {
                size_t flen = rings_[i]->getExpectedPacketSize();
                size_t skipBytes = batchSize_ * flen;
                std::vector<uint8_t> junk(skipBytes);
                rings_[i]->readData(junk.data(), skipBytes);  // advance readTotal past this batch
                lastWritePos_[i].store((int64_t)rings_[i]->getReadTotal());
                lastPeekedReadPos_[i] = (int64_t)rings_[i]->getReadTotal() - 1;
            }
            return;
        }
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
        lastPeekedReadPos_[i] = (int64_t)rings_[i]->getReadTotal();
    }
}
