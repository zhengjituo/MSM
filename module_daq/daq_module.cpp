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
        onStart();
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

void DAQModule::onStart() {
    if (readerRunning_.load()) {
        std::cerr << "[DAQModule] Already running\n";
        return;
    }

    CxDeviceSetSdkOption("WithLog", "false");
    CxDeviceSdkInit();

    CxChassisHandle chassis = nullptr;
    CxCardHandle* cards = nullptr;
    uint32_t card_count = 0;
    CxDeviceGetChassis(PXIe, &chassis);
    CxDeviceGetCards(chassis, &cards, &card_count);

    card_ = nullptr;
    for (uint32_t i = 0; i < card_count; i++) {
        uint32_t did = 0;
        CxDeviceGetCardDeviceId(cards[i], &did);
        if (did == 0x1073) {
            card_ = cards[i];
            break;
        }
    }
    if (!card_) {
        std::cerr << "[DAQModule] No 1073 card found\n";
        CxDeviceSdkDeinit();
        return;
    }

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
    }

    for (int i = 0; i < NCH; i++) {
        int ch = CHANNELS[i];
        CxDeviceSetChannelRecvBufferSize(card_, ch, 16 * 1024 * 1024);
        CxDeviceStartChannelRecv(card_, ch);
        CxDeviceSetChannel(card_, ch, "run");
    }

    for (int i = 0; i < NCH; i++)
        totalFrames_[i].store(0);

    readerRunning_ = true;
    readerThread_ = std::thread(&DAQModule::readerLoop, this);

    std::cout << "[DAQModule] Started, 4 channels running\n";
}

void DAQModule::onStop() {
    if (!readerRunning_.load()) return;

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

    while (readerRunning_.load()) {
        for (int i = 0; i < NCH; i++) {
            int ch = CHANNELS[i];
            void* frames = nullptr;
            uint32_t n = 0;
            CxDeviceReadChannelFrames(card_, ch, &frames, &n);
            if (n == 0) continue;

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
            totalFrames_[i].fetch_add(written / flen);
        }
    }
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
