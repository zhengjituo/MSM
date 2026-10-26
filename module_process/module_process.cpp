#include "module_process.h"
#include "MessageBus.h"
#include "NIStringParser.h"
#include <iostream>
#include <cstring>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <time.h>
#include <sys/time.h>

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string base64Encode(const void* data, size_t len) {
    const uint8_t* in = (const uint8_t*)data;
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16);
        if (i + 1 < len) v |= ((uint32_t)in[i + 1] << 8);
        if (i + 2 < len) v |= in[i + 2];
        out += kB64[(v >> 18) & 0x3F];
        out += kB64[(v >> 12) & 0x3F];
        out += (i + 1 < len) ? kB64[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? kB64[v & 0x3F] : '=';
    }
    return out;
}

static std::string fmtDouble(double v, int prec = 6) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(prec) << v;
    return oss.str();
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
        lastSumH_[i].store(0.0);
        lastX_[i].store(0.0);
        lastY_[i].store(0.0);
        lastI_[i].store(0.0);
        batchData_[i].trig_id.reserve(batchSize_);
        batchData_[i].bunch_id.reserve(batchSize_);
        batchData_[i].min_val.reserve(batchSize_);
        batchData_[i].max_val.reserve(batchSize_);
        batchData_[i].sum_l.reserve(batchSize_);
        batchData_[i].sum_h.reserve(batchSize_);
        batchData_[i].vpp.reserve(batchSize_);
        batchData_[i].x.reserve(batchSize_);
        batchData_[i].y.reserve(batchSize_);
        batchData_[i].i.reserve(batchSize_);
    }
    idleDelayMs_ = 10;
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

double ModuleProcess::getLastSumH(int ch) const {
    if (ch < 0 || ch >= nCh_) return 0.0;
    return lastSumH_[ch].load();
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
    if (idx < 0)
        std::cout << "[" << name_ << "] FrameIndex set to auto\n";
    else
        std::cout << "[" << name_ << "] FrameIndex set to " << idx << "\n";
}

void ModuleProcess::onIdle() {
    if ((consuming_.load() || flushing_.load()) && !emergencyStop_.load()) {
        doConsumeOnce();
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
        int idx = -1;
        if (msg.data.contains("index"))
            idx = static_cast<int>(msg.data.getInt("index"));
        setFrameIndex(idx);
    }
}

void ModuleProcess::onConfigure(const msm::Json& data) {
    if (data.contains("BatchSize"))
        batchSize_ = static_cast<size_t>(data.getInt("BatchSize"));
    if (data.contains("FrameSize"))
        frameSize_ = static_cast<size_t>(data.getInt("FrameSize"));
    if (data.contains("StorageTarget"))
        storageTarget_ = data.get("StorageTarget");
    if (data.contains("BunchIdStep"))
        bunchIdStep_ = static_cast<int>(data.getInt("BunchIdStep"));
    std::cout << "[" << name_ << "] Configured: BatchSize=" << batchSize_
              << " FrameSize=" << frameSize_
              << " StorageTarget=" << storageTarget_
              << " BunchIdStep=" << bunchIdStep_ << "\n";
}

void ModuleProcess::onStart() {
    emergencyStop_ = false;
    flushing_ = false;
    for (int i = 0; i < nCh_; i++) {
        processedFrames_[i].store(0);
        processedBatches_[i].store(0);
        lastWritePos_[i].store(rings_[i] ? rings_[i]->getWriteTotal() : 0);
        lastTrigId_[i].store(0);
        lastBunchId_[i].store(0);
        lastSumH_[i].store(0.0);
        lastX_[i].store(0.0);
        lastY_[i].store(0.0);
        lastI_[i].store(0.0);
    }
    bunchIdStep_ = 10;
    consuming_ = true;
    for (int i = 0; i < nCh_; i++) lastPeekedReadPos_[i] = -1;
    std::cout << "[" << name_ << "] Full processing (message-driven, " << nCh_
              << " ch, batch=" << batchSize_ << ")\n";
}

void ModuleProcess::onStop() {
    if (flushing_.load() || emergencyStop_.load())
        return;
    consuming_ = false;
    flushing_ = true;
    std::cout << "[" << name_ << "] Draining remaining frames...\n";
    // keep doConsumeOnce running until ring is empty
    // emergencyStop_ set by doConsumeOnce when flush completes
}

void ModuleProcess::onStopImmediate() {
    consuming_ = false;
    flushing_ = false;
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
        response.data.set("ch" + std::to_string(i + 1) + "_sum_h",
                          fmtDouble(lastSumH_[i].load()));
    }
    response.data.set("x_pos", fmtDouble(lastX_[0].load()));
    response.data.set("y_pos", fmtDouble(lastY_[0].load()));
    response.data.set("intensity", fmtDouble(lastI_[0].load()));
    response.data.set("frame_index", std::to_string(frameIndex_.load()));

    // Feature table data from last batch
    {
        std::lock_guard<std::mutex> lock(batchMutex_);
        if (batchData_[0].count > 0) {
            response.data.set("batch_no", std::to_string(batchData_[0].batchNo));
            for (int i = 0; i < nCh_; i++) {
                if (batchData_[i].count == 0) continue;
                std::string p = "ch" + std::to_string(i + 1) + "_feat_";
                response.data.set(p + "trig_id", std::to_string(batchData_[i].trig_id[0]));
                response.data.set(p + "bunch_id", std::to_string(batchData_[i].bunch_id[0]));
                response.data.set(p + "min", fmtDouble(batchData_[i].min_val[0]));
                response.data.set(p + "max", fmtDouble(batchData_[i].max_val[0]));
                response.data.set(p + "sum_l", fmtDouble(batchData_[i].sum_l[0]));
                response.data.set(p + "sum_h", fmtDouble(batchData_[i].sum_h[0]));
                response.data.set(p + "vpp", fmtDouble(batchData_[i].vpp[0]));
                response.data.set(p + "x", fmtDouble(batchData_[i].x[0]));
                response.data.set(p + "y", fmtDouble(batchData_[i].y[0]));
                response.data.set(p + "i", fmtDouble(batchData_[i].i[0]));
                if (!batchData_[i].wave_raw.empty()) {
                    std::string raw_bytes(
                        (const char*)batchData_[i].wave_raw.data(),
                        batchData_[i].wave_raw.size() * sizeof(uint16_t));
                    response.data.set(p + "wave_raw", base64Encode(raw_bytes.data(), raw_bytes.size()));
                    response.data.set(p + "wave_offset", std::to_string(batchData_[i].wave_offset));
                    response.data.set(p + "wave_scale", std::to_string(batchData_[i].wave_scale));
                    response.data.set(p + "wave_range", std::to_string(batchData_[i].wave_range));
                }
            }
        }
    }
}

void ModuleProcess::registerVars() {
    for (int i = 0; i < MAX_CH; i++) {
        std::string idx = std::to_string(i + 1);
        declareVar("ch" + idx + "_frames", processedFrames_[i]);
        declareVar("ch" + idx + "_batches", processedBatches_[i]);
        declareVar("ch" + idx + "_trig_id", lastTrigId_[i]);
        declareVar("ch" + idx + "_bunch_id", lastBunchId_[i]);
        declareVar("ch" + idx + "_sum_h", lastSumH_[i]);
        if (i == 0) {
            declareVar("x_pos", lastX_[i]);
            declareVar("y_pos", lastY_[i]);
            declareVar("intensity", lastI_[i]);
        }
    }
}

void ModuleProcess::extractFrame(const uint8_t* frame, size_t flen, int ch) {
    FrameHeader hdr;
    if (!parseHeader(frame, flen, hdr)) return;

    const uint8_t* trailerData = frame + flen - TRAILER_SIZE;
    FrameTrailer tlr;
    if (!parseTrailer(trailerData, TRAILER_SIZE, tlr)) return;

    CalibratedFrame cal = calibrate(hdr, tlr);

    batchData_[ch].trig_id[0] = hdr.trig_id;
    batchData_[ch].bunch_id[0] = (uint64_t)cal.bunch_id;
    batchData_[ch].min_val[0] = cal.min_val;
    batchData_[ch].max_val[0] = cal.max_val;
    batchData_[ch].sum_l[0] = cal.sum_l;
    batchData_[ch].sum_h[0] = cal.sum_h;
    batchData_[ch].vpp[0] = cal.vpp;
    batchData_[ch].x[0] = cal.x;
    batchData_[ch].y[0] = cal.y;
    batchData_[ch].i[0] = cal.i;

    auto wraw = extractWaveformRaw(frame, flen, hdr);
    batchData_[ch].wave_raw = std::move(wraw.samples);
    batchData_[ch].wave_offset = wraw.offset;
    batchData_[ch].wave_scale = wraw.scale;
    batchData_[ch].wave_range = wraw.range;

    lastTrigId_[ch].store((int64_t)hdr.trig_id);
    lastBunchId_[ch].store((int64_t)cal.bunch_id);
    lastSumH_[ch].store(cal.sum_h);
    if (ch == 0) {
        lastX_[0].store(cal.x);
        lastY_[0].store(cal.y);
        lastI_[0].store(cal.i);
    }
}

int ModuleProcess::detectBunchIdStep(const uint8_t* batchBuf, size_t flen, size_t nFrames) const {
    if (nFrames < 2) return 1;
    uint64_t b0 = 0, b1 = 0;
    for (size_t i = 0; i < nFrames; i++) {
        const uint8_t* f = batchBuf + i * flen;
        if (f[0] != 0xEB || f[1] != 0x91) continue;
        const uint8_t* tr = f + flen - TRAILER_SIZE;
        uint64_t bid = readU48BE(tr + 2);
        if (b0 == 0) {
            b0 = bid;
        } else if (bid != b0) {
            b1 = bid;
            break;
        }
    }
    if (b0 == 0 || b1 == 0) return 1;
    int64_t diff = (int64_t)(b1 - b0);
    if (diff < 0) diff = -diff;
    return (diff > 0 && diff <= 1000) ? (int)diff : 10;
}

void ModuleProcess::doConsumeOnce() {
    if (emergencyStop_.load()) return;

    // Drain mode: read ALL remaining frames from all channels, any amount
    if (flushing_.load()) {
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

            if (saveRing_)
                saveRing_->writeFromDevice(batchBuf_.data(), got);

            size_t actualFrames = got / flen;
            processedFrames_[i].fetch_add(actualFrames);
            lastWritePos_[i].store(lastWritePos_[i].load() + got);
            std::cout << "[" << name_ << "] ch" << (i + 1)
                      << " flush=" << actualFrames << " -> saveRing\n";
        }
        if (allEmpty) {
            flushing_ = false;
            emergencyStop_ = true;
            std::cout << "[" << name_ << "] flush done:";
            for (int i = 0; i < nCh_; i++)
                std::cout << " ch" << (i + 1) << "=" << processedFrames_[i].load();
            std::cout << std::endl;
        }
        return;
    }

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

    int64_t curBatch = processedBatches_[0].load() + 1;

    // Check saveRing space before reading: need room for all channels
    size_t totalNeeded = 0;
    for (int i = 0; i < nCh_; i++) {
        size_t flen = rings_[i]->getExpectedPacketSize();
        totalNeeded += batchSize_ * flen;
    }
    if (saveRing_ && saveRing_->availableSpace() < totalNeeded) {
        return;
    }

    // 先读所有通道数据到 batchBuf_（ch1,ch2..chN 顺序排列）
    size_t totalBytes = 0;
    size_t offsets[MAX_CH];
    for (int i = 0; i < nCh_; i++) {
        size_t flen = rings_[i]->getExpectedPacketSize();
        size_t batchBytes = batchSize_ * flen;
        offsets[i] = totalBytes;
        totalBytes += batchBytes;
        if (batchBuf_.size() < totalBytes)
            batchBuf_.resize(totalBytes);

        size_t got = rings_[i]->readData(batchBuf_.data() + offsets[i], batchBytes);
        if (got < batchBytes) {
            std::cerr << "[" << name_ << "] ch" << (i + 1)
                      << " read " << got << " < " << batchBytes << "\n";
            totalBytes = 0;
            break;
        }
    }

    // 一次性写入 saveRing
    if (totalBytes > 0 && saveRing_)
        saveRing_->writeFromDevice(batchBuf_.data(), totalBytes);

    if (totalBytes == 0) return;

    // Detect bunchId step from ch0
    size_t flen0 = rings_[0] ? rings_[0]->getExpectedPacketSize() : 0;
    if (flen0 > 0) {
        int detected = detectBunchIdStep(batchBuf_.data() + offsets[0], flen0, batchSize_);
        if (detected > 0) bunchIdStep_ = detected;
    }

    // Determine frame index: auto-align if frameIndex_ < 0
    // 扫 ch0 批内所有帧，选 bunch_id 最靠近 100000 整数边界（%100000==0）的帧，
    // 使显示的特征值落在整 100ms 时刻（100000 bunch = 10000 触发 @100kHz）。
    // 不用恒定步长公式：束流 bunch 结构变化时步进不是恒定 10（实测有 6~10 波动），
    // 公式会偏几十 ms；逐帧扫描对任意步进都稳健
    int selIdx = frameIndex_.load();
    uint64_t alignDist = UINT64_MAX;   // 选中帧到 100000 边界的距离（bunch 单位）
    if (selIdx < 0) {
        bool found = false;
        if (flen0 > 0 && flen0 >= TRAILER_SIZE) {
            for (size_t i = 0; i < batchSize_; i++) {
                const uint8_t* ff = batchBuf_.data() + offsets[0] + i * flen0;
                if (ff[0] != 0xEB || ff[1] != 0x91) continue;
                uint64_t bid = readU48BE(ff + flen0 - TRAILER_SIZE + 2);
                if (bid == 0) continue;
                uint64_t m = bid % 100000ULL;
                uint64_t dist = m < 50000ULL ? m : 100000ULL - m;
                if (dist < alignDist) {
                    alignDist = dist;
                    selIdx = (int)i;
                    found = true;
                }
            }
        }
        if (!found) selIdx = 0;
    }
    if (selIdx < 0) selIdx = 0;
    if ((size_t)selIdx >= batchSize_) selIdx = 0;
    if (alignDist == UINT64_MAX && nCh_ > 0 && flen0 >= TRAILER_SIZE) {
        // 手动 idx：按选中帧的 bunch_id 计算偏差（仅日志用）
        const uint8_t* ff = batchBuf_.data() + offsets[0] + (size_t)selIdx * flen0;
        if (ff[0] == 0xEB && ff[1] == 0x91) {
            uint64_t b = readU48BE(ff + flen0 - TRAILER_SIZE + 2);
            uint64_t m = b % 100000ULL;
            alignDist = m < 50000ULL ? m : 100000ULL - m;
        }
    }

    // Extract selected frame from each channel
    for (int i = 0; i < nCh_; i++) {
        size_t flen = rings_[i]->getExpectedPacketSize();
        const uint8_t* frame = batchBuf_.data() + offsets[i] + (size_t)selIdx * flen;
        extractFrame(frame, flen, i);
        batchData_[i].count = 1;
        batchData_[i].batchNo = curBatch;
    }

    // Print per-channel log line (first frame bunch_id)
    std::string ts;
    if (getenv("MSM_DAQ_TS")) {
        char tb[32];
        time_t tw = time(nullptr);
        struct tm lt; localtime_r(&tw, &lt);
        struct timeval tv; gettimeofday(&tv, nullptr);
        strftime(tb, sizeof(tb), "%H:%M:%S", &lt);
        ts = std::string("[ts ") + tb + "." + std::to_string(tv.tv_usec / 1000) + "] ";
    }
    for (int i = 0; i < nCh_; i++) {
        size_t flen = rings_[i]->getExpectedPacketSize();
        uint64_t fb = 0;
        const uint8_t* ff = batchBuf_.data() + offsets[i];
        if (ff[0] == 0xEB && ff[1] == 0x91 && flen >= TRAILER_SIZE) {
            fb = readU48BE(ff + flen - TRAILER_SIZE + 2);
        }
        int chPhys = (i < (int)physChannels_.size()) ? physChannels_[i] : (i + 1);
        std::cout << ts << "[" << name_ << "] ch" << chPhys
                  << " batch=" << curBatch
                  << " first_bunch_id=" << fb
                  << " idx=" << selIdx
                  << " target_bunch_id=" << (uint64_t)batchData_[i].bunch_id[0]
                  << "\n";
    }

    // Print feature table (nCh_ 通道，动态)
    {
        std::cout << ts << "[" << name_ << "] batch=" << curBatch
                  << " frame_idx=" << selIdx << " step=" << bunchIdStep_
                  << " dist=" << (alignDist == UINT64_MAX ? 0 : alignDist) << "\n";
        auto printRow = [&](const char* label, std::vector<double> BatchFrameData::*field) {
            std::cout << "  " << label;
            for (int i = 0; i < nCh_; i++)
                std::cout << "  " << std::fixed << std::setprecision(6) << (batchData_[i].*field)[0];
            std::cout << "\n";
        };
        std::cout << "  trig_id";
        for (int i = 0; i < nCh_; i++) std::cout << "  " << batchData_[i].trig_id[0];
        std::cout << "\n";
        std::cout << "  bunch_id";
        for (int i = 0; i < nCh_; i++) std::cout << "  " << batchData_[i].bunch_id[0];
        std::cout << "\n";
        printRow("min",      &BatchFrameData::min_val);
        printRow("max",      &BatchFrameData::max_val);
        printRow("sum_l",    &BatchFrameData::sum_l);
        printRow("sum_h",    &BatchFrameData::sum_h);
        printRow("vpp",      &BatchFrameData::vpp);
        printRow("x",        &BatchFrameData::x);
        printRow("y",        &BatchFrameData::y);
        printRow("i",        &BatchFrameData::i);
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
