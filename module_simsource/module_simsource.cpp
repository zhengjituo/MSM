#include "module_simsource.h"
#include "MessageBus.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>
#include <algorithm>

// ── 来自真实采集卡 (TrigLength=512, frameSize=1152) ─────────
// Header: bytes 0-63, TrigID@17-20 zeroed (填帧时写入)
static const uint8_t kFrameHeader[] = {
    0xeb, 0x91, 0x00, 0x00, 0x04, 0x80, 0x00, 0x10,
    0x73, 0x00, 0x00, 0x03, 0xe8, 0x10, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x17, 0xe7, 0xd0, 0xd8,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0xbe,
    0x17, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x03,
    0xe8, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static constexpr size_t kHeaderSize = 64;

// Trailer: 最后 64 字节, BunchID@+2..+7 zeroed
static const uint8_t kFrameTrailer[] = {
    0xeb, 0x92, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x1b, 0xf4, 0x1c, 0x3e, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x38, 0x2f, 0x1c, 0x00, 0x4a, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static constexpr size_t kTrailerSize = 64;

constexpr int ModuleSim::MAX_CH;

ModuleSim::ModuleSim()
    : Module("DAQModule")
{
    vars_.parent = this;
    idleDelayMs_ = 10;
    frameSize_ = trigLength_ * 2 + 128;
    frameBuf_.resize(frameSize_);
}

ModuleSim::~ModuleSim() {
    stop();
}

msm::ModuleVars* ModuleSim::getVars() {
    return &vars_;
}

UserCircularBuffer* ModuleSim::getRing(int idx) {
    if (idx < 0 || idx >= (int)rings_.size()) return nullptr;
    return rings_[idx];
}

ModuleSim::Stats ModuleSim::getStats() const {
    Stats s;
    s.is_running = generating_.load();
    for (int i = 0; i < nCh_; i++) {
        s.total_frames.push_back(totalFrames_[i].load());
        s.overflow_count.push_back(
            rings_[i] ? rings_[i]->getOverflowCount() : 0);
        s.frame_size.push_back(frameSize_);
    }
    return s;
}

void ModuleSim::onIdle() {
    if (generating_.load() && !emergencyStop_.load()) {
        msm::Message gen("GenerateOnce");
        post(gen);
    }
}

void ModuleSim::handleState(const msm::Message& msg) {
    if (msg.cmd == "GenerateOnce") {
        if (!emergencyStop_.load())
            doGenerateOnce();

    } else if (msg.cmd == "Configure") {
        onConfigure(msg.data);
        if (!msg.getSender().empty()) {
            msm::Message response("Response");
            response.setTarget(msg.getSender());
            response.setCorrelationId(msg.getCorrelationId());
            response.data.set("status", "ok");
            msm::MessageBus::instance().send(response);
        }
    } else if (msg.cmd == "Start") {
        onStart();
    } else if (msg.cmd == "Stop") {
        onStop();
    } else if (msg.cmd == "Reset") {
        onReset();
    }
}

void ModuleSim::onConfigure(const msm::Json& data) {
    size_t newTrigLen = trigLength_;
    size_t newFrames = framesPerCycle_;
    size_t newRate = triggerRateHz_;
    int newNCh = nCh_;

    if (data.contains("TrigLength"))
        newTrigLen = (size_t)data.getInt("TrigLength");
    if (data.contains("FramesPerCycle"))
        newFrames = (size_t)data.getInt("FramesPerCycle");
    if (data.contains("TriggerRateHz"))
        newRate = (size_t)data.getInt("TriggerRateHz");
    if (data.contains("NumChannels"))
        newNCh = data.getInt("NumChannels");

    nCh_ = std::min(newNCh, MAX_CH);
    trigLength_ = newTrigLen;
    if (data.contains("WaveDataEnable"))
        waveDataEnable_ = data.get("WaveDataEnable");
    frameSize_ = (waveDataEnable_ == "0") ? 128 : trigLength_ * 2 + 128;
    framesPerCycle_ = newFrames;
    triggerRateHz_ = newRate;
    idleDelayMs_ = std::max<size_t>(1, framesPerCycle_ * 1000 / triggerRateHz_);
    bunchInc_ = std::max<size_t>(1, 1000000 / triggerRateHz_);
    frameBuf_.resize(frameSize_);

    for (auto* r : rings_)
        r->setExpectedPacketSize(frameSize_);
    nextTrigId_ = 1;
    nextBunchId_ = 1;

    std::cout << "[DAQModule] configured: " << nCh_ << "ch"
              << " trigLen=" << trigLength_
              << " frameSize=" << frameSize_
              << " wde=" << waveDataEnable_
              << " fps=" << (framesPerCycle_ * 1000 / idleDelayMs_)
              << " bunchInc=" << bunchInc_
              << " (" << triggerRateHz_ / 1000.0 << " kHz)\n";
}

void ModuleSim::onStart() {
    if (generating_.load()) {
        std::cerr << "[DAQModule] Already running\n";
        return;
    }
    if (nCh_ == 0) {
        std::cerr << "[DAQModule] No channels configured\n";
        return;
    }

    for (int i = 0; i < nCh_; i++) {
        rings_[i]->setExpectedPacketSize(frameSize_);
        totalFrames_[i].store(0);
    }
    nextTrigId_ = 1;
    nextBunchId_ = 1;

    emergencyStop_ = false;
    generating_ = true;
    std::cout << "[DAQModule] Generating " << framesPerCycle_
              << " frames/cycle (" << (framesPerCycle_ * 1000 / idleDelayMs_)
              << " fps), " << nCh_ << "ch, bunchInc=" << bunchInc_ << "\n";
}

void ModuleSim::onStop() {
    generating_ = false;
    emergencyStop_ = true;
    std::cout << "[DAQModule] Stopped";
    for (int i = 0; i < nCh_; i++)
        std::cout << " ch" << (i + 1) << "=" << totalFrames_[i].load();
    std::cout << "\n";
}

void ModuleSim::onReset() {
    for (int i = 0; i < nCh_; i++) {
        totalFrames_[i].store(0);
        if (rings_[i]) rings_[i]->resetOverflow();
    }
    nextTrigId_ = 1;
    nextBunchId_ = 1;
    std::cout << "[DAQModule] Reset\n";
}

void ModuleSim::onStatus(msm::Message& response) {
    Stats s = getStats();
    for (int i = 0; i < (int)s.total_frames.size(); i++) {
        response.data.set("ch" + std::to_string(i + 1) + "_frames",
                          std::to_string(s.total_frames[i]));
        response.data.set("ch" + std::to_string(i + 1) + "_overflow",
                          std::to_string(s.overflow_count[i]));
        response.data.set("ch" + std::to_string(i + 1) + "_frame_size",
                          std::to_string(s.frame_size[i]));
    }
    response.data.set("running", s.is_running ? "true" : "false");
}

// ── 用真实帧模板构造一帧 ────────────────────────────────────
// 所有通道共享同一份帧数据, ch 参数预留

void ModuleSim::buildFrame(uint8_t* buf, size_t frameSizeP, int /*ch*/,
                            uint64_t trigId, uint64_t bunchId) {
    // 前 kHeaderSize 字节：拷贝 header 模板 + 填 TrigID
    std::memcpy(buf, kFrameHeader, kHeaderSize);
    // 修正包长（有可能 TrigLength 变化）
    uint32_t plen = (uint32_t)frameSizeP;
    buf[2] = (uint8_t)(plen >> 24);
    buf[3] = (uint8_t)(plen >> 16);
    buf[4] = (uint8_t)(plen >> 8);
    buf[5] = (uint8_t)(plen);
    // TrigID
    buf[17] = (uint8_t)(trigId >> 24);
    buf[18] = (uint8_t)(trigId >> 16);
    buf[19] = (uint8_t)(trigId >> 8);
    buf[20] = (uint8_t)(trigId);

    // 波形数据：16 位递增斜坡（大端，与 frame_parser 的 readU16BE 一致）。
    // 注意：不能按"每个字节 +1"来填，那样两个字节拼成 16 位数后每 128 个
    // 采样就会回绕一次（65280 → 258），画出来是锯齿而非递增。
    {
        size_t nSamples = (frameSizeP - kHeaderSize - kTrailerSize) / 2;
        for (size_t i = 0; i < nSamples; i++) {
            uint16_t v = (uint16_t)(i * 128);      // 0 .. 65408，单调递增
            buf[kHeaderSize + i * 2]     = (uint8_t)(v >> 8);
            buf[kHeaderSize + i * 2 + 1] = (uint8_t)(v & 0xFF);
        }
    }

    // 尾段
    size_t trailerOff = frameSizeP - kTrailerSize;
    std::memcpy(buf + trailerOff, kFrameTrailer, kTrailerSize);
    // BunchID 在 trailer offset +2 (6 bytes)
    uint8_t* bidPos = buf + trailerOff + 2;
    bidPos[0] = (uint8_t)(bunchId >> 40);
    bidPos[1] = (uint8_t)(bunchId >> 32);
    bidPos[2] = (uint8_t)(bunchId >> 24);
    bidPos[3] = (uint8_t)(bunchId >> 16);
    bidPos[4] = (uint8_t)(bunchId >> 8);
    bidPos[5] = (uint8_t)(bunchId);
}

// ── 每 IDLE 周期生成 framesPerCycle_ 帧 ──────────────────────

void ModuleSim::doGenerateOnce() {
    if (emergencyStop_.load() || nCh_ == 0) return;

    for (size_t f = 0; f < framesPerCycle_; f++) {
        uint64_t trigId = nextTrigId_++;
        uint64_t bunchId = nextBunchId_;
        nextBunchId_ += bunchInc_;

        buildFrame(frameBuf_.data(), frameSize_, 0, trigId, bunchId);

        for (int ch = 0; ch < nCh_; ch++) {
            if (emergencyStop_.load()) return;
            size_t written = rings_[ch]->writeFromDevice(frameBuf_.data(), frameSize_);
            totalFrames_[ch].fetch_add(written / frameSize_);
        }
    }
}

// ── SimVars ────────────────────────────────────────────────────

void* ModuleSim::SimVars::get(const std::string& name) {
    return nullptr;
}

bool ModuleSim::SimVars::set(const std::string& name, const std::string& value) {
    return false;
}

std::map<std::string, std::string> ModuleSim::SimVars::list() {
    return {};
}
