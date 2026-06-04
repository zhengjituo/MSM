#include "daq_module.h"
#include "MessageBus.h"
#include "CxDeviceCore.h"
#include "CxDeviceInternal.h"
#include "CxDefine.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>
#include <csignal>
#include <algorithm>


DAQModule::DAQModule()
    : Module("DAQModule")
{
    vars_.parent = this;
}

DAQModule::~DAQModule() {
    stop();
    for (auto* r : rings_) delete r;
    rings_.clear();
}

msm::ModuleVars* DAQModule::getVars() {
    return &vars_;
}

UserCircularBuffer* DAQModule::getRing(int idx) {
    if (idx < 0 || idx >= (int)rings_.size()) return nullptr;
    return rings_[idx];
}

DAQModule::Stats DAQModule::getStats() const {
    Stats s;
    s.is_running = readerRunning_.load();
    for (int i = 0; i < activeChCount_; i++) {
        const auto& slot = activeChannels_[i];
        s.total_frames.push_back(slot.totalFrames.load());
        s.overflow_count.push_back(slot.ring ? slot.ring->getOverflowCount() : 0);
        s.frame_size.push_back(slot.frameSize.load());
    }
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

void DAQModule::loadConfig(const std::string& path) {
    if (!config_.loadFromJson(path)) {
        std::cerr << "[DAQModule] Failed to load config from " << path << "\n";
        return;
    }

    int totalCh = 0;
    for (const auto& cardCfg : config_.cards) {
        if (cardCfg.card_type != "1073") continue;
        for (const auto& chCfg : cardCfg.channels)
            if (chCfg.runEnable) totalCh++;
    }

    if (totalCh > MAX_CH) {
        std::cerr << "[DAQModule] WARN: " << totalCh << " channels > MAX_CH="
                  << MAX_CH << ", truncating\n";
        totalCh = MAX_CH;
    }

    for (int i = 0; i < totalCh; i++)
        rings_.push_back(new UserCircularBuffer(256U * 1024U * 1024U, 384));

    std::cout << "[DAQModule] Config loaded: " << totalCh << " ring(s) created\n";
}

void DAQModule::onConfigure(const msm::Json& data) {
    for (auto& cardCfg : config_.cards) {
        if (cardCfg.card_type != "1073") continue;
        for (auto& ch : cardCfg.channels) {
            if (!ch.runEnable) continue;
            if (data.contains("TrigLength"))
                ch.trig_length = data.get("TrigLength");
            if (data.contains("VoltageRange"))
                ch.voltage_range = data.get("VoltageRange");
            if (data.contains("TrigLevel"))
                ch.trig_level = data.get("TrigLevel");
            if (data.contains("TrigSource"))
                ch.trig_source = data.get("TrigSource");
            if (data.contains("TrigMode"))
                ch.trig_mode = data.get("TrigMode");
            if (data.contains("TrigPulseWidth"))
                ch.trig_pulse_width = data.get("TrigPulseWidth");
            if (data.contains("TrigPolarity"))
                ch.trig_polarity = data.get("TrigPolarity");
            if (data.contains("TrigDelayTime"))
                ch.trig_delay_time = data.get("TrigDelayTime");
            if (data.contains("TrigPosition"))
                ch.trig_position = data.get("TrigPosition");
            if (data.contains("VoltageOffset"))
                ch.voltage_offset = data.get("VoltageOffset");
            if (data.contains("DeadTime"))
                ch.dead_time = data.get("DeadTime");
            if (data.contains("FiniteTrigNum"))
                ch.finite_trig_num = data.get("FiniteTrigNum");
            if (data.contains("WaveDataEnable"))
                ch.wave_data_enable = data.get("WaveDataEnable");
            if (data.contains("WorkMode"))
                ch.work_mode = data.get("WorkMode");
            if (data.contains("ExternalTrigPolarity"))
                ch.external_trig_polarity = data.get("ExternalTrigPolarity");
        }
    }
    std::cout << "[DAQModule] Configured from message\n";
}

bool DAQModule::onStart() {
    if (readerRunning_.load()) {
        std::cerr << "[DAQModule] Already running\n";
        return false;
    }

    // ── 扫描硬件 ────────────────────────────────────────────
    std::cout << "[DAQModule] Initializing SDK...\n";
    CxDeviceSetSdkOption("WithLog", "false");
    CxDeviceSdkInit();

    CxChassisHandle chassis = nullptr;
    CxCardHandle* hwCards = nullptr;
    uint32_t hwCount = 0;
    CxDeviceGetChassis(PXIe, &chassis);
    CxDeviceGetCards(chassis, &hwCards, &hwCount);
    std::cout << "[DAQModule] Found " << hwCount << " cards in chassis\n";

    cards1073_.clear();
    card3010_ = nullptr;
    for (uint32_t i = 0; i < hwCount; i++) {
        uint32_t did = 0;
        CxDeviceGetCardDeviceId(hwCards[i], &did);
        if (did == 0x1073)
            cards1073_.push_back(hwCards[i]);
        else if (did == 0x3010)
            card3010_ = hwCards[i];
    }
    std::cout << "[DAQModule] " << cards1073_.size() << " x 1073, "
              << (card3010_ ? 1 : 0) << " x 3010\n";

    // 按 slot 号升序排序（确保匹配顺序与物理槽位一致）
    std::sort(cards1073_.begin(), cards1073_.end(), [](void* a, void* b) {
        uint32_t sa = 0, sb = 0;
        CxDeviceGetCardSlotNo(a, &sa);
        CxDeviceGetCardSlotNo(b, &sb);
        return sa < sb;
    });

    // ── 逐配置初始化 ────────────────────────────────────────
    activeChCount_ = 0;
    int cardIdx = 0;

    for (const auto& cardCfg : config_.cards) {
        if (cardCfg.card_type != "1073") continue;

        if (cardIdx >= (int)cards1073_.size()) {
            std::cerr << "[DAQModule] Not enough 1073 cards, remaining configs skipped\n";
            break;
        }
        void* hCard = cards1073_[cardIdx++];

        CxDeviceSetCard(hCard, "open");
        CxDeviceSetCardValue(hCard, "ClockSource", "0");
        CxDeviceSetCard(hCard, "init");
        CxDeviceSetCardValue(hCard, "SyncWindowEnable", "0");
        CxDeviceSetCardValue(hCard, "SyncSource", "0");
        std::cout << "[DAQModule] Card 1073 #" << cardIdx
                  << " (config slot=" << cardCfg.slot << ") initializing\n";

        for (const auto& chCfg : cardCfg.channels) {
            if (!chCfg.runEnable) {
                std::cout << "[DAQModule]   ch" << chCfg.channel << " disabled\n";
                continue;
            }
            if (activeChCount_ >= MAX_CH) {
                std::cerr << "[DAQModule]   ch" << chCfg.channel
                          << " dropped (MAX_CH=" << MAX_CH << ")\n";
                break;
            }
            int ch = chCfg.channel;
            CxDeviceSetChannelValue(hCard, ch, "GlobalTrigEnable", "1");
            CxDeviceSetChannelValue(hCard, ch, "TrigSource", chCfg.trig_source.c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigMode", chCfg.trig_mode.c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigLength", chCfg.trig_length.c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigPulseWidth", chCfg.trig_pulse_width.c_str());
            CxDeviceSetChannelValue(hCard, ch, "VoltageRange", chCfg.voltage_range.c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigLevel", chCfg.trig_level.c_str());
            std::cout << "[DAQModule]   ch" << ch << " configured ("
                      << "TrigLength=" << chCfg.trig_length
                      << " VoltageRange=" << chCfg.voltage_range
                      << " TrigLevel=" << chCfg.trig_level
                      << " TrigSource=" << chCfg.trig_source << ")\n";

            auto& slot = activeChannels_[activeChCount_++];
            slot.cardHandle = hCard;
            slot.ch = ch;
            slot.ring = rings_[activeChCount_ - 1];
            slot.totalFrames.store(0);
            slot.frameSize.store(0);
        }
    }

    if (activeChCount_ == 0) {
        std::cerr << "[DAQModule] No active channels to start\n";
        CxDeviceSdkDeinit();
        return false;
    }
    std::cout << "[DAQModule] " << activeChCount_ << " active channel(s)\n";

    // ── 启动接收 ────────────────────────────────────────────
    for (int i = 0; i < activeChCount_; i++) {
        auto& slot = activeChannels_[i];
        CxDeviceSetChannelRecvBufferSize(slot.cardHandle, slot.ch, 16 * 1024 * 1024);
        CxDeviceStartChannelRecv(slot.cardHandle, slot.ch);
        CxDeviceSetChannel(slot.cardHandle, slot.ch, "run");
        std::cout << "[DAQModule] ch" << slot.ch << " (id=" << i << ") recv started\n";
    }

    std::cout << "[DAQModule] Settling 1073 (2s)...\n";
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // ── 启动 reader ─────────────────────────────────────────
    readerRunning_ = true;
    readerThread_ = std::thread(&DAQModule::readerLoop, this);
    std::cout << "[DAQModule] Reader thread started (" << activeChCount_ << " ch)\n";

    // ── 3010 trigger on ────────────────────────────────────
    if (card3010_) {
        if (init3010()) {
            setTriggerOutput(true);
            std::cout << "[DAQModule] 3010 trigger enabled\n";
        }
    } else {
        std::cout << "[DAQModule] No 3010 card, trigger not started\n";
    }
    return true;
}

void DAQModule::stop() {
    if (readerRunning_.load() || card3010_ || !cards1073_.empty())
        onStop();
    Module::stop();
}

void DAQModule::onStop() {
    if (card3010_) {
        setTriggerOutput(false);
        std::cout << "[DAQModule] 3010 trigger off, waiting 200ms...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    flushAll();

    if (card3010_) {
        CxDeviceSetCard(card3010_, "close");
        card3010_ = nullptr;
        std::cout << "[DAQModule] 3010 closed\n";
    }

    readerRunning_ = false;
    if (readerThread_.joinable())
        readerThread_.join();

    cleanup();
    std::cout << "[DAQModule] Stopped\n";
}

void DAQModule::onReset() {
    for (int i = 0; i < activeChCount_; i++) {
        activeChannels_[i].totalFrames.store(0);
        if (activeChannels_[i].ring)
            activeChannels_[i].ring->resetOverflow();
    }
    std::cout << "[DAQModule] Stats reset\n";
}

void DAQModule::onStatus(msm::Message& response) {
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

void DAQModule::readerLoop() {
    std::vector<uint8_t> buf(4U * 1024U * 1024U);

    while (readerRunning_.load()) {
        bool any_data = false;
        for (int i = 0; i < activeChCount_; i++) {
            auto& slot = activeChannels_[i];
            void* frames = nullptr;
            uint32_t n = 0;
            CxDeviceReadChannelFrames(slot.cardHandle, slot.ch, &frames, &n);
            if (n == 0) continue;

            any_data = true;

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

            slot.frameSize.store(flen);
            slot.ring->setExpectedPacketSize(flen);
            size_t written = slot.ring->writeFromDevice(buf.data(), bytes);
            int64_t total = slot.totalFrames.fetch_add(written / flen) + (int64_t)(written / flen);

            std::cout << "[reader] ch" << slot.ch << " id=" << i
                      << " n=" << n
                      << " flen=" << flen
                      << " written=" << (written / flen)
                      << " total=" << total << std::endl;
        }
        if (!any_data)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void DAQModule::flushAll() {
    if (activeChCount_ == 0) return;
    std::vector<uint8_t> buf(4U * 1024U * 1024U);

    for (int i = 0; i < activeChCount_; i++) {
        auto& slot = activeChannels_[i];
        void* frames = nullptr;
        uint32_t n = 0;
        CxDeviceReadChannelFrames(slot.cardHandle, slot.ch, &frames, &n);
        if (n == 0) continue;

        auto* rp = static_cast<RawPack*>(frames);
        size_t flen = rp[0].BufferSize;
        if (flen == 0) continue;
        size_t bytes = (size_t)n * flen;
        if (bytes > buf.size()) buf.resize(bytes);
        uint8_t* dst = buf.data();
        for (uint32_t j = 0; j < n; j++) {
            if (!rp[j].Buffer) { bytes = 0; break; }
            std::memcpy(dst, rp[j].Buffer, flen);
            dst += flen;
        }
        if (bytes == 0) continue;

        slot.frameSize.store(flen);
        slot.ring->setExpectedPacketSize(flen);
        size_t written = slot.ring->writeFromDevice(buf.data(), bytes);
        slot.totalFrames.fetch_add(written / flen);
        std::cout << "[flush] ch" << slot.ch << " id=" << i
                  << " n=" << n << " flen=" << flen
                  << " written=" << (written / flen) << std::endl;
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
    if (cards1073_.empty()) return;

    for (int i = 0; i < activeChCount_; i++) {
        auto& slot = activeChannels_[i];
        CxDeviceStopChannelRecv(slot.cardHandle, slot.ch);
        CxDeviceSetChannel(slot.cardHandle, slot.ch, "stop");
    }

    for (auto h : cards1073_)
        CxDeviceSetCard(h, "close");

    CxDeviceSdkDeinit();
    cards1073_.clear();
    activeChCount_ = 0;
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
