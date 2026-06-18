#include "module_daq.h"
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


ModuleDAQ::ModuleDAQ()
    : Module("DAQModule")
{
    vars_.parent = this;
    idleDelayMs_ = 10;
}

ModuleDAQ::~ModuleDAQ() {
    stop();
    for (auto* r : rings_) delete r;
    rings_.clear();
}

msm::ModuleVars* ModuleDAQ::getVars() {
    return &vars_;
}

UserCircularBuffer* ModuleDAQ::getRing(int idx) {
    if (idx < 0 || idx >= (int)rings_.size()) return nullptr;
    return rings_[idx];
}

ModuleDAQ::Stats ModuleDAQ::getStats() const {
    Stats s;
    s.is_running = acquiring_.load();
    for (int i = 0; i < activeChCount_; i++) {
        const auto& slot = activeChannels_[i];
        s.total_frames.push_back(slot.totalFrames.load());
        s.overflow_count.push_back(slot.ring ? slot.ring->getOverflowCount() : 0);
        s.frame_size.push_back(slot.frameSize.load());
    }
    return s;
}

void ModuleDAQ::onIdle() {
    if (acquiring_.load() && !emergencyStop_.load()) {
        msm::Message readOnce("ReadOnce");
        post(readOnce);
    }
}

void ModuleDAQ::handleState(const msm::Message& msg) {
    if (msg.cmd == "ReadOnce") {
        if (!emergencyStop_.load())
            doReadOnce();

    } else if (msg.cmd == "Configure") {
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

void ModuleDAQ::onConfigure(const msm::Json& data) {
    // 遍历 data map，按前缀拆解到 card/channel
    bool needRingCreate = false;

    for (auto it = data.data.begin(); it != data.data.end(); ++it) {
        const std::string& key = it->first;
        const std::string& val = it->second;
        // Card_X_Type / Card_X_Slot / Card_X_ChCount
        if (key.find("Card_") == 0) {
            size_t p1 = key.find('_', 5);
            if (p1 == std::string::npos) continue;
            int cardIdx = std::stoi(key.substr(5, p1 - 5));
            std::string rest = key.substr(p1 + 1);

            if (rest == "ChCount") {
                // Card count changed, need to rebuild config structure
                int newCount = std::stoi(val);
                while ((int)config_.cards.size() < cardIdx) {
                    config_.cards.push_back(CardConfig());
                }
                config_.cards[cardIdx - 1].channels.resize(std::stoi(val));
                needRingCreate = true;
                continue;
            }

            // Card_X_Field
            int ci = cardIdx - 1;
            if (ci < 0 || ci >= (int)config_.cards.size()) continue;
            auto& card = config_.cards[ci];

            if (rest == "Index") { card.card_index = std::stoi(val); continue; }
            if (rest == "Type")  { card.card_type = val; needRingCreate = true; continue; }
            if (rest == "Slot")  { card.slot = std::stoi(val); continue; }

            // Card_X_Ch_Y_Field
            size_t chSep = rest.find("Ch_");
            if (chSep != 0) continue;
            size_t p2 = rest.find('_', 3);
            if (p2 == std::string::npos) continue;
            int chIdx = std::stoi(rest.substr(3, p2 - 3)) - 1;
            std::string field = rest.substr(p2 + 1);

            if (chIdx < 0 || chIdx >= (int)card.channels.size()) continue;
            auto& ch = card.channels[chIdx];

            if (field == "Enable")     { ch.runEnable = (val == "1"); needRingCreate = true; continue; }
            if (field == "Channel")    { ch.channel = std::stoi(val); continue; }
            if (field == "CardIndex")  { ch.cardIndex = std::stoi(val); continue; }
            if (field == "Slot")       { ch.slot = std::stoi(val); continue; }

            if (field == "TrigSource")           { ch.trig_source = val; continue; }
            if (field == "TrigMode")             { ch.trig_mode = val; continue; }
            if (field == "TrigLength")           { ch.trig_length = val; continue; }
            if (field == "TrigLevel")            { ch.trig_level = val; continue; }
            if (field == "TrigPulseWidth")       { ch.trig_pulse_width = val; continue; }
            if (field == "TrigPolarity")         { ch.trig_polarity = val; continue; }
            if (field == "TrigDelayTime")        { ch.trig_delay_time = val; continue; }
            if (field == "TrigPosition")         { ch.trig_position = val; continue; }
            if (field == "TrigWindowStart")      { ch.trig_window_start = val; continue; }
            if (field == "VoltageRange")         { ch.voltage_range = val; continue; }
            if (field == "VoltageOffset")        { ch.voltage_offset = val; continue; }
            if (field == "DeadTime")             { ch.dead_time = val; continue; }
            if (field == "FiniteTrigNum")        { ch.finite_trig_num = val; continue; }
            if (field == "WaveDataEnable")       { ch.wave_data_enable = val; continue; }
            if (field == "WorkMode")             { ch.work_mode = val; continue; }
            if (field == "K")                    { ch.k = val; continue; }
            if (field == "K1st")                 { ch.k1st = val; continue; }
            if (field == "Kx")                   { ch.kx = val; continue; }
            if (field == "Ky")                   { ch.ky = val; continue; }
            if (field == "OffsetX")              { ch.offset_x = val; continue; }
            if (field == "OffsetY")              { ch.offset_y = val; continue; }
            if (field == "B1st")                 { ch.b1st = val; continue; }
            if (field == "ValueThresholdHigh")   { ch.value_threshold_high = val; continue; }
            if (field == "ValueThresholdLow")    { ch.value_threshold_low = val; continue; }
            if (field == "ExternalTrigPolarity") { ch.external_trig_polarity = val; continue; }
            if (field == "IpGlobalRST")          { ch.ip_global_rst = val; continue; }
            if (field == "IpLogicRST")           { ch.ip_logic_rst = val; continue; }
        }
    }

    // 如果有结构性变更（card/channel 数量变化），重建 ring
    if (needRingCreate && !configured_.load()) {
        for (auto* r : rings_) delete r;
        rings_.clear();

        int totalCh = 0;
        for (const auto& cardCfg : config_.cards) {
            if (cardCfg.card_type != "1073") continue;
            for (const auto& chCfg : cardCfg.channels)
                if (chCfg.runEnable) totalCh++;
        }
        if (totalCh > MAX_CH) totalCh = MAX_CH;

        for (int i = 0; i < totalCh; i++)
            rings_.push_back(new UserCircularBuffer(256U * 1024U * 1024U, 384));

        configured_ = true;
        std::cout << "[ModuleDAQ] Config loaded: " << totalCh << " ring(s)\n";
    }
}

bool ModuleDAQ::onStart() {
    if (acquiring_.load()) {
        std::cerr << "[ModuleDAQ] Already running\n";
        return false;
    }

    // ── 扫描硬件 ────────────────────────────────────────────
    std::cout << "[ModuleDAQ] Initializing SDK...\n";
    CxDeviceSetSdkOption("WithLog", "false");
    CxDeviceSdkInit();

    CxChassisHandle chassis = nullptr;
    CxCardHandle* hwCards = nullptr;
    uint32_t hwCount = 0;
    CxDeviceGetChassis(PXIe, &chassis);
    CxDeviceGetCards(chassis, &hwCards, &hwCount);
    std::cout << "[ModuleDAQ] Found " << hwCount << " cards in chassis\n";

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
    std::cout << "[ModuleDAQ] " << cards1073_.size() << " x 1073, "
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
            std::cerr << "[ModuleDAQ] Not enough 1073 cards, remaining configs skipped\n";
            break;
        }
        void* hCard = cards1073_[cardIdx++];

        CxDeviceSetCard(hCard, "open");
        CxDeviceSetCardValue(hCard, "ClockSource", "0");
        CxDeviceSetCard(hCard, "init");
        CxDeviceSetCardValue(hCard, "SyncWindowEnable", "0");
        CxDeviceSetCardValue(hCard, "SyncSource", "0");
        std::cout << "[ModuleDAQ] Card 1073 #" << cardIdx
                  << " (config slot=" << cardCfg.slot << ") initializing\n";

        for (const auto& chCfg : cardCfg.channels) {
            if (!chCfg.runEnable) {
                std::cout << "[ModuleDAQ]   ch" << chCfg.channel << " disabled\n";
                continue;
            }
            if (activeChCount_ >= MAX_CH) {
                std::cerr << "[ModuleDAQ]   ch" << chCfg.channel
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
            std::cout << "[ModuleDAQ]   ch" << ch << " configured ("
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
        std::cerr << "[ModuleDAQ] No active channels to start\n";
        CxDeviceSdkDeinit();
        return false;
    }
    std::cout << "[ModuleDAQ] " << activeChCount_ << " active channel(s)\n";

    // ── 启动接收 ────────────────────────────────────────────
    for (int i = 0; i < activeChCount_; i++) {
        auto& slot = activeChannels_[i];
        CxDeviceSetChannelRecvBufferSize(slot.cardHandle, slot.ch, 16 * 1024 * 1024);
        CxDeviceStartChannelRecv(slot.cardHandle, slot.ch);
        CxDeviceSetChannel(slot.cardHandle, slot.ch, "run");
        std::cout << "[ModuleDAQ] ch" << slot.ch << " (id=" << i << ") recv started\n";
    }

    std::cout << "[ModuleDAQ] Settling 1073 (2s)...\n";
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // ── 3010 trigger on + 标记采集 ─────────────────────────
    if (card3010_) {
        if (init3010()) {
            setTriggerOutput(true);
            std::cout << "[ModuleDAQ] 3010 trigger enabled\n";
        }
    } else {
        std::cout << "[ModuleDAQ] No 3010 card, trigger not started\n";
    }

    emergencyStop_ = false;
    acquiring_ = true;
    std::cout << "[ModuleDAQ] Acquiring (message-driven)\n";
    return true;
}

void ModuleDAQ::stop() {
    onStop();
    Module::stop();
}

void ModuleDAQ::onStop() {
    if (!acquiring_.exchange(false) && !emergencyStop_.load())
        return;
    emergencyStop_ = true;

    if (card3010_) {
        setTriggerOutput(false);
        std::cout << "[ModuleDAQ] 3010 trigger off, waiting 200ms...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    flushAll();

    if (card3010_) {
        CxDeviceSetCard(card3010_, "close");
        card3010_ = nullptr;
        std::cout << "[ModuleDAQ] 3010 closed\n";
    }

    doCleanup();
    std::cout << "[ModuleDAQ] Stopped\n";
}

void ModuleDAQ::onReset() {
    for (int i = 0; i < activeChCount_; i++) {
        activeChannels_[i].totalFrames.store(0);
        if (activeChannels_[i].ring)
            activeChannels_[i].ring->resetOverflow();
    }
    std::cout << "[ModuleDAQ] Stats reset\n";
}

void ModuleDAQ::onStatus(msm::Message& response) {
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

void ModuleDAQ::doReadOnce() {
    if (emergencyStop_.load() || activeChCount_ == 0) return;

    for (int i = 0; i < activeChCount_; i++) {
        if (emergencyStop_.load()) return;

        auto& slot = activeChannels_[i];
        void* frames = nullptr;
        uint32_t n = 0;
        CxDeviceReadChannelFrames(slot.cardHandle, slot.ch, &frames, &n);
        if (n == 0) continue;

        auto* rp = static_cast<RawPack*>(frames);
        size_t flen = rp[0].BufferSize;
        if (flen == 0) continue;
        size_t bytes = (size_t)n * flen;

        thread_local std::vector<uint8_t> buf;
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

        // [reader] print suppressed — see consumer onBatch for progress
    }
}

void ModuleDAQ::flushAll() {
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
        size_t ovf = slot.ring->getOverflowCount();
        std::cout << "[flush] ch" << slot.ch << " id=" << i
                  << " n=" << n << " flen=" << flen
                  << " written=" << (written / flen)
                  << " ovf=" << ovf << std::endl;
    }
}

bool ModuleDAQ::init3010() {
    if (!card3010_) return false;

    CxDeviceSetCard(card3010_, "open");
    CxDeviceSetCard(card3010_, "init");
    CxDeviceSetCardValue(card3010_, "SyncTrigTrigMux", "2");
    CxDeviceSetCardValue(card3010_, "SyncTrigGlbSync", "1");
    CxDeviceSetCardValue(card3010_, "TrigSource", "2");
    CxDeviceSetCardValue(card3010_, "SoftwareSync", "1");

    std::cout << "[ModuleDAQ] 3010 initialized\n";
    return true;
}

bool ModuleDAQ::setTriggerOutput(bool enable) {
    if (!card3010_) return false;
    uint32_t val = enable ? 1 : 0;
    int ret = CxDeviceWriteReg(card3010_, 0x1C020, val);
    if (ret != 0) {
        std::cerr << "[ModuleDAQ] 3010 reg write failed: " << ret << "\n";
        return false;
    }
    return true;
}

void ModuleDAQ::doCleanup() {
    if (cards1073_.empty()) return;

    for (int i = 0; i < activeChCount_; i++) {
        auto& slot = activeChannels_[i];
        if (emergencyStop_.load()) break;
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

void* ModuleDAQ::DAQVars::get(const std::string& name) {
    return nullptr;
}

bool ModuleDAQ::DAQVars::set(const std::string& name, const std::string& value) {
    return false;
}

std::map<std::string, std::string> ModuleDAQ::DAQVars::list() {
    return {};
}
