#include "daq_module.h"
#include "MessageBus.h"
#include "CxDeviceCore.h"
#include "CxDeviceInternal.h"
#include "CxDefine.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <csignal>

constexpr int DAQModule::CHANNELS[4];

DAQModule::DAQModule()
    : Module("DAQModule")
{
    for (int i = 0; i < NCH; i++)
        rings_[i] = new UserCircularBuffer(256U * 1024U * 1024U, 384);
    vars_.parent = this;
}

DAQModule::~DAQModule() {
    stop();
    for (int i = 0; i < NCH; i++)
        delete rings_[i];
}

msm::ModuleVars* DAQModule::getVars() {
    return &vars_;
}

UserCircularBuffer* DAQModule::getRing(int channelIndex) {
    if (channelIndex < 0 || channelIndex >= NCH) return nullptr;
    return rings_[channelIndex];
}

DAQModule::Stats DAQModule::getStats() const {
    Stats s;
    for (int i = 0; i < NCH; i++) {
        s.total_frames[i] = totalFrames_[i].load();
        s.overflow_count[i] = rings_[i]->getOverflowCount();
        s.frame_size[i] = frameSize_[i].load();
    }
    s.is_running = readerRunning_.load();
    return s;
}

void DAQModule::handleState(const msm::Message& msg) {
    if (msg.cmd == "Configure") {
        onConfigure(msg.data);
    } else if (msg.cmd == "Start") {
        bool started = onStart();
        if (started) {
            msm::Message response("Response");
            response.setTarget(msg.getSender());
            response.setCorrelationId(msg.getCorrelationId());
            response.data.set("status", "ok");
            msm::MessageBus::instance().send(response);
        }

    } else if (msg.cmd == "Stop") {
        onStop();
    } else if (msg.cmd == "Reset") {
        onReset();
    } else if (msg.cmd == "Status") {
        msm::Message response("Response");
        response.setTarget(msg.getSender());
        response.setCorrelationId(msg.getCorrelationId());
        onStatus(response);
        msm::MessageBus::instance().send(response);
    }
}

void DAQModule::onConfigure(const msm::Json& data) {
    if (data.contains("TrigLength"))
        trigLength_ = data.getInt("TrigLength");
    if (data.contains("VoltageRange"))
        voltageRange_ = data.get("VoltageRange");
    if (data.contains("TrigLevel"))
        trigLevel_ = data.getInt("TrigLevel");
    if (data.contains("TrigSource"))
        trigSource_ = data.get("TrigSource");
    std::cout << "[DAQModule] Configured: TrigLength=" << trigLength_
              << " VoltageRange=" << voltageRange_
              << " TrigLevel=" << trigLevel_
              << " TrigSource=" << trigSource_ << std::endl;
}

bool DAQModule::onStart() {
    if (readerRunning_.load()) {
        std::cerr << "[DAQModule] Already running\n";
        return false;
    }

    std::cout << "[DAQModule] Initializing SDK...\n";
    CxDeviceSetSdkOption("WithLog", "false");
    CxDeviceSdkInit();

    CxChassisHandle chassis = nullptr;
    CxCardHandle* cards = nullptr;
    uint32_t card_count = 0;
    CxDeviceGetChassis(PXIe, &chassis);
    CxDeviceGetCards(chassis, &cards, &card_count);
    std::cout << "[DAQModule] Found " << card_count << " cards\n";

    card_ = nullptr;
    card3010_ = nullptr;
    for (uint32_t i = 0; i < card_count; i++) {
        uint32_t did = 0;
        CxDeviceGetCardDeviceId(cards[i], &did);
        if (did == 0x1073) card_ = cards[i];
        else if (did == 0x3010) card3010_ = cards[i];
    }
    if (!card_) {
        std::cerr << "[DAQModule] No 1073 card found\n";
        CxDeviceSdkDeinit();
        return false;
    }

    // ── 1073 init ──────────────────────────────────────────
    CxDeviceSetCard(card_, "open");
    CxDeviceSetCardValue(card_, "ClockSource", "0");
    CxDeviceSetCard(card_, "init");
    CxDeviceSetCardValue(card_, "SyncWindowEnable", "0");
    CxDeviceSetCardValue(card_, "SyncSource", "0");

    for (int i = 0; i < NCH; i++) {
        int ch = CHANNELS[i];
        CxDeviceSetChannelValue(card_, ch, "GlobalTrigEnable", "1");
        CxDeviceSetChannelValue(card_, ch, "TrigSource", trigSource_.c_str());
        CxDeviceSetChannelValue(card_, ch, "TrigMode", "2");
        CxDeviceSetChannelValue(card_, ch, "TrigLength", std::to_string(trigLength_).c_str());
        CxDeviceSetChannelValue(card_, ch, "TrigPulseWidth", "16");
        CxDeviceSetChannelValue(card_, ch, "VoltageRange", voltageRange_.c_str());
        CxDeviceSetChannelValue(card_, ch, "TrigLevel", std::to_string(trigLevel_).c_str());
        std::cout << "[DAQModule] ch" << ch << " configured\n";
    }

    for (int i = 0; i < NCH; i++) {
        int ch = CHANNELS[i];
        CxDeviceSetChannelRecvBufferSize(card_, ch, 16 * 1024 * 1024);
        CxDeviceStartChannelRecv(card_, ch);
        CxDeviceSetChannel(card_, ch, "run");
        std::cout << "[DAQModule] ch" << ch << " recv started\n";
    }

    // Wait for 1073 to settle
    std::cout << "[DAQModule] Waiting for 1073 to stabilize...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    for (int i = 0; i < NCH; i++)
        totalFrames_[i].store(0);

    // Start reader BEFORE trigger — 立即开始轮询，一旦触发就有数据
    readerRunning_ = true;
    readerThread_ = std::thread(&DAQModule::readerLoop, this);
    std::cout << "[DAQModule] Reader thread started\n";

    // ── 3010 init + trigger on ─────────────────────────────
    if (card3010_) {
        if (init3010()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            setTriggerOutput(true);
            std::cout << "[DAQModule] 3010 trigger enabled\n";
        }
    } else {
        std::cout << "[DAQModule] No 3010 card, trigger not started\n";
    }
    return true;
}

void DAQModule::onStop() {
    if (!readerRunning_.load()) return;

    // Stop triggers first
    if (card3010_) {
        setTriggerOutput(false);
        CxDeviceSetCard(card3010_, "close");
        std::cout << "[DAQModule] 3010 trigger disabled\n";
    }

    readerRunning_ = false;
    if (readerThread_.joinable())
        readerThread_.join();

    cleanup();
    std::cout << "[DAQModule] Stopped\n";
}

void DAQModule::onReset() {
    for (int i = 0; i < NCH; i++) {
        totalFrames_[i].store(0);
        rings_[i]->resetOverflow();
    }
    std::cout << "[DAQModule] Stats reset\n";
}

void DAQModule::onStatus(msm::Message& response) {
    Stats s = getStats();
    for (int i = 0; i < NCH; i++) {
        response.data.set("ch" + std::to_string(CHANNELS[i]) + "_frames",
                          std::to_string(s.total_frames[i]));
        response.data.set("ch" + std::to_string(CHANNELS[i]) + "_overflow",
                          std::to_string(s.overflow_count[i]));
        response.data.set("ch" + std::to_string(CHANNELS[i]) + "_frame_size",
                          std::to_string(s.frame_size[i]));
    }
    response.data.set("running", s.is_running ? "true" : "false");
}

void DAQModule::readerLoop() {
    std::vector<uint8_t> buf(4U * 1024U * 1024U);
    int idle_count = 0;

    while (readerRunning_.load()) {
        for (int i = 0; i < NCH; i++) {
            int ch = CHANNELS[i];
            void* frames = nullptr;
            uint32_t n = 0;
            CxDeviceReadChannelFrames(card_, ch, &frames, &n);
            if (n == 0) continue;

            idle_count = 0;

            auto* rp = static_cast<RawPack*>(frames);
            size_t flen = rp[0].BufferSize;
            if (flen == 0) continue;
            size_t bytes = (size_t)n * flen;

            if (bytes > buf.size())
                buf.resize(bytes);
            uint8_t* dst = buf.data();
            for (uint32_t j = 0; j < n; j++) {
                if (!rp[j].Buffer) { bytes = 0; break; }
                std::memcpy(dst, rp[j].Buffer, flen);
                dst += flen;
            }
            if (bytes == 0) continue;

            frameSize_[i].store(flen);
            rings_[i]->setExpectedPacketSize(flen);
            size_t written = rings_[i]->writeFromDevice(buf.data(), bytes);
            int64_t total = totalFrames_[i].fetch_add(written / flen) + (int64_t)(written / flen);
            std::cout << "[reader] ch" << ch << " n=" << n
                      << " flen=" << flen
                      << " written=" << (written / flen)
                      << " total=" << total << std::endl;
        }
        idle_count++;
        if (idle_count == 100) {
            std::cout << "[reader] No data after 100 iterations\n";
        } else if (idle_count == 1000) {
            std::cout << "[reader] No data after 1000 iterations\n";
        }
    }
}

bool DAQModule::init3010() {
    if (!card3010_) return false;

    CxDeviceSetCard(card3010_, "open");
    CxDeviceSetCard(card3010_, "init");
    CxDeviceSetCardValue(card3010_, "SyncTrigTrigMux", "2");
    CxDeviceSetCardValue(card3010_, "SyncTrigGlbSync", "1");
    CxDeviceSetCardValue(card3010_, "TrigSource", "2");
    CxDeviceSetCardValue(card3010_, "SoftwareSync", "1");

    std::cout << "[DAQModule] 3010 initialized\n";
    return true;
}

bool DAQModule::setTriggerOutput(bool enable) {
    if (!card3010_) return false;
    uint32_t val = enable ? 1 : 0;
    int ret = CxDeviceWriteReg(card3010_, 0x1C020, val);
    if (ret != 0) {
        std::cerr << "[DAQModule] 3010 reg write failed: " << ret << "\n";
        return false;
    }
    return true;
}

void DAQModule::cleanup() {
    if (!card_) return;

    for (int i = 0; i < NCH; i++) {
        CxDeviceStopChannelRecv(card_, CHANNELS[i]);
        CxDeviceSetChannel(card_, CHANNELS[i], "stop");
    }
    CxDeviceSetCard(card_, "close");
    CxDeviceSdkDeinit();
    card_ = nullptr;
}

// ── DAQVars ─────────────────────────────────────────────────────

void* DAQModule::DAQVars::get(const std::string& name) {
    return nullptr;
}

bool DAQModule::DAQVars::set(const std::string& name, const std::string& value) {
    return false;
}

std::map<std::string, std::string> DAQModule::DAQVars::list() {
    return {};
}
