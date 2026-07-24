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
#include <cmath>
#include <cstdio>
#include <climits>


ModuleDAQ::ModuleDAQ()
    : Module("DAQModule")
{
    vars_.parent = this;
    idleDelayMs_ = 10;
}

ModuleDAQ::~ModuleDAQ() {
    stop();
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
        s.channel_ids.push_back(slot.ch);
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

    } else if (msg.cmd == "Init") {
        if (emergencyStop_.load()) emergencyStop_ = false;
        onReadConfig(msg.data);
        // reload: hw 已初始化，只需推送参数，不关闭重开
        if (!hwReady_.load())
            initHW();
        updateConfig();
    } else if (msg.cmd == "Configure" || msg.cmd == "ReadConfig") {
        onReadConfig(msg.data);
    } else if (msg.cmd == "InitHW") {
        initHW();
    } else if (msg.cmd == "UpdateConfig") {
        updateConfig();

    } else if (msg.cmd == "CloseHW") {
        closeHW();

    } else if (msg.cmd == "Start") {
        if (!hwReady_.load()) {
            std::cerr << "[ModuleDAQ] HW not initialized, call InitHW first\n";
            return;
        }
        bool started = onStart();
        {
            msm::Message response("Response");
            response.setTarget(msg.getSender());
            response.setCorrelationId(msg.getCorrelationId());
            response.data.set("status", started ? "ok" : "error");
            msm::MessageBus::instance().send(response);
        }

    } else if (msg.cmd == "Stop") {
        if (hwReady_.load()) onStop();
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

void ModuleDAQ::onReadConfig(const msm::Json& data) {
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

    if (needRingCreate)
        configured_ = true;
}

// ── 固定点小数 → 十六进制（抄自 card_parameter_define.cpp） ──
static std::string fixedPointHex2(double value) {
    // Q16.16: scale = 2^16 = 65536
    int64_t scaled = (int64_t)std::llround(value * 65536.0);
    int32_t clamped = (int32_t)std::max(std::min(scaled, (int64_t)INT_MAX), (int64_t)INT_MIN);
    uint32_t u = (uint32_t)clamped;
    char buf[16]; snprintf(buf, sizeof(buf), "0x%08X", u);
    return buf;
}
static std::string fixedPointHex3(double value) {
    // Q8.24: scale = 2^24 = 16777216
    int64_t scaled = (int64_t)std::llround(value * 16777216.0);
    int32_t clamped = (int32_t)std::max(std::min(scaled, (int64_t)INT_MAX), (int64_t)INT_MIN);
    uint32_t u = (uint32_t)clamped;
    char buf[16]; snprintf(buf, sizeof(buf), "0x%08X", u);
    return buf;
}

// ── UpdateConfig：将 DAQ_Config 推送到硬件通道寄存器 ─────
bool ModuleDAQ::updateConfig() {
    if (!hwReady_.load()) {
        std::cerr << "[ModuleDAQ] updateConfig skipped: HW not ready\n";
        return false;
    }
    activeChCount_ = 0;

    // ── 板卡级参数配置（每卡一次） ─────────────────────────
    for (auto h : cards1073_) {
        CxDeviceSetCardValue(h, "ClockSource", "0");
        CxDeviceSetCardValue(h, "SyncWindowEnable", "0");
        CxDeviceSetCardValue(h, "SyncSource", "0");
    }
    if (card3010_) {
        CxDeviceSetCardValue(card3010_, "SyncTrigTrigMux", "2");
        CxDeviceSetCardValue(card3010_, "SyncTrigGlbSync", "1");
        CxDeviceSetCardValue(card3010_, "TrigSource", "2");
        CxDeviceSetCardValue(card3010_, "SoftwareSync", "1");
        std::cout << "[ModuleDAQ] 3010 card params set\n";
    }

    // ── 通道级参数配置 ─────────────────────────────────────
    for (const auto& cardCfg : config_.cards) {
        if (cardCfg.card_type != "1073") continue;

        void* hCard = nullptr;
        for (auto h : cards1073_) {
            uint32_t slot = 0;
            CxDeviceGetCardSlotNo(h, &slot);
            if ((int)slot == cardCfg.slot) { hCard = h; break; }
        }
        if (!hCard) {
            std::cerr << "[ModuleDAQ] Card slot " << cardCfg.slot << " not found\n";
            continue;
        }
        std::cout << "[ModuleDAQ] Configuring 1073 slot=" << cardCfg.slot << "\n";

        for (const auto& chCfg : cardCfg.channels) {
            if (!chCfg.runEnable) continue;
            if (activeChCount_ >= MAX_CH) break;

            int ch = chCfg.channel;
            CxDeviceSetChannelValue(hCard, ch, "GlobalTrigEnable", "1");
            CxDeviceSetChannelValue(hCard, ch, "TrigSource", chCfg.trig_source.c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigMode", chCfg.trig_mode.c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigLength", chCfg.trig_length.c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigPulseWidth", chCfg.trig_pulse_width.c_str());
            CxDeviceSetChannelValue(hCard, ch, "VoltageRange", chCfg.voltage_range.c_str());
            CxDeviceSetChannelValue(hCard, ch, "VoltageOffset", chCfg.voltage_offset.c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigLevel", chCfg.trig_level.c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigPolarity", chCfg.trig_polarity.c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigPosition", chCfg.trig_position.c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigWindowStart", chCfg.trig_window_start.c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigDelayTime",
                std::to_string(config_.trig_delay_time_fix + std::stoi(chCfg.trig_delay_time)).c_str());
            CxDeviceSetChannelValue(hCard, ch, "ExternalTrigPolarity", chCfg.external_trig_polarity.c_str());
            CxDeviceSetChannelValue(hCard, ch, "DeadTime", chCfg.dead_time.c_str());
            CxDeviceSetChannelValue(hCard, ch, "FiniteTrigNum", chCfg.finite_trig_num.c_str());
            CxDeviceSetChannelValue(hCard, ch, "WorkMode", chCfg.work_mode.c_str());
            CxDeviceSetChannelValue(hCard, ch, "WaveDataEnable", chCfg.wave_data_enable.c_str());
            CxDeviceSetChannelValue(hCard, ch, "ValueThresholdHigh", chCfg.value_threshold_high.c_str());
            CxDeviceSetChannelValue(hCard, ch, "ValueThresholdLow", chCfg.value_threshold_low.c_str());
            CxDeviceSetChannelValue(hCard, ch, "IpGlobalRST", chCfg.ip_global_rst.c_str());
            CxDeviceSetChannelValue(hCard, ch, "IpLogicRST", chCfg.ip_logic_rst.c_str());
            if (!chCfg.k1st.empty())
                CxDeviceSetChannelValue(hCard, ch, "EffectiveDataExtractionK1st",
                    fixedPointHex3(std::stod(chCfg.k1st)).c_str());
            if (!chCfg.b1st.empty())
                CxDeviceSetChannelValue(hCard, ch, "EffectiveDataExtractionB1st",
                    fixedPointHex3(std::stod(chCfg.b1st)).c_str());
            // Kx/Ky/K/OffsetX/OffsetY 仅通道1
            if (ch == 1) {
                if (!chCfg.kx.empty())
                    CxDeviceSetChannelValue(hCard, ch, "EffectiveDataExtractionKX",
                        fixedPointHex2(std::stod(chCfg.kx)).c_str());
                if (!chCfg.ky.empty())
                    CxDeviceSetChannelValue(hCard, ch, "EffectiveDataExtractionKY",
                        fixedPointHex2(std::stod(chCfg.ky)).c_str());
                if (!chCfg.k.empty())
                    CxDeviceSetChannelValue(hCard, ch, "EffectiveDataExtractionK",
                        fixedPointHex2(std::stod(chCfg.k)).c_str());
                if (!chCfg.offset_x.empty())
                    CxDeviceSetChannelValue(hCard, ch, "EffectiveDataExtractionOffsetX",
                        fixedPointHex2(std::stod(chCfg.offset_x)).c_str());
                if (!chCfg.offset_y.empty())
                    CxDeviceSetChannelValue(hCard, ch, "EffectiveDataExtractionOffsetY",
                        fixedPointHex2(std::stod(chCfg.offset_y)).c_str());
            }
            std::cout << "[ModuleDAQ]   ch" << ch << " configured\n";

            auto& slot = activeChannels_[activeChCount_++];
            slot.cardHandle = hCard;
            slot.ch = ch;
            slot.ring = rings_[activeChCount_ - 1];
            slot.totalFrames.store(0);
            slot.frameSize.store(0);
        }
    }

    if (activeChCount_ == 0) {
        std::cerr << "[ModuleDAQ] No active channels configured\n";
        return false;
    }
    std::cout << "[ModuleDAQ] " << activeChCount_ << " active channel(s)"
              << "  init OK, ready for start\n";
    return true;
}

bool ModuleDAQ::onStart() {
    bool expected = false;
    if (!acquiring_.compare_exchange_strong(expected, true)) {
        std::cerr << "[ModuleDAQ] Already running\n";
        return false;
    }

    // 先关掉 acquiring_，避免启动过程中 doReadOnce 把旧数据写进 ring
    acquiring_ = false;

    // ── 启动接收 ────────────────────────────────────────────
    for (int i = 0; i < activeChCount_; i++) {
        auto& slot = activeChannels_[i];
        CxDeviceSetChannelRecvBufferSize(slot.cardHandle, slot.ch, 16 * 1024 * 1024);
        CxDeviceStartChannelRecv(slot.cardHandle, slot.ch);
        CxDeviceSetChannel(slot.cardHandle, slot.ch, "run");
        std::cout << "[ModuleDAQ] ch" << slot.ch << " (id=" << i << ") recv started\n";
    }

    // ── 3010 trigger ON（等所有通道启动稳定再开触发） ────
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    setTriggerOutput(true);
    std::cout << "[ModuleDAQ] 3010 trigger enabled\n";

    emergencyStop_ = false;
    acquiring_ = true;
    for (int i = 0; i < activeChCount_; i++)
        activeChannels_[i].totalFrames.store(0);
    std::cout << "[ModuleDAQ] Acquiring (message-driven)\n";

    // DAQ ready → start Process consumer from clean ring state
    msm::Message startProcess("Start");
    startProcess.setTarget("Process");
    msm::MessageBus::instance().send(startProcess);

    return true;
}

void ModuleDAQ::stop() {
    onStop();
    if (hwReady_.load()) closeHW();
    Module::stop();
}

void ModuleDAQ::onStop() {
    if (!acquiring_.load() && emergencyStop_.load())
        return;
    if (!acquiring_.exchange(false) && !emergencyStop_.load())
        return;
    emergencyStop_ = true;

    setTriggerOutput(false);
    std::cout << "[ModuleDAQ] 3010 trigger off\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    stopChannels();
    flushAll();
    std::cout << "[DAQ] stop:";
    for (int i = 0; i < activeChCount_; i++)
        std::cout << " ch" << activeChannels_[i].ch << "=" << activeChannels_[i].totalFrames.load();
    std::cout << std::endl;
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

        // 读帧量在 Process/Storage 端打印，此处静默
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

void ModuleDAQ::stopChannels() {
    for (int i = 0; i < activeChCount_; i++) {
        auto& slot = activeChannels_[i];
        CxDeviceStopChannelRecv(slot.cardHandle, slot.ch);
        CxDeviceSetChannel(slot.cardHandle, slot.ch, "stop");
    }
}

// ── 硬件初始化（一次，含自检校准）─────────────────────────────
bool ModuleDAQ::initHW() {
    if (hwReady_.load()) {
        std::cout << "[ModuleDAQ] Reload: closing cards for re-init...\n";
        if (card3010_) {
            CxDeviceSetCard(card3010_, "close");
            card3010_ = nullptr;
        }
        for (auto h : cards1073_)
            CxDeviceSetCard(h, "close");
        cards1073_.clear();
        // SDK 保持初始化，不重新 deinit/init
    } else {
        std::cout << "[ModuleDAQ] Initializing SDK...\n";
        CxDeviceSetSdkOption("WithLog", "false");
        if (CxDeviceSdkInit() != 0) {
            std::cerr << "[ModuleDAQ] SDK init failed\n";
            return false;
        }
    }

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

    std::sort(cards1073_.begin(), cards1073_.end(), [](void* a, void* b) {
        uint32_t sa = 0, sb = 0;
        CxDeviceGetCardSlotNo(a, &sa);
        CxDeviceGetCardSlotNo(b, &sb);
        return sa < sb;
    });

    for (auto h : cards1073_) {
        uint32_t slot = 0;
        CxDeviceGetCardSlotNo(h, &slot);
        CxDeviceSetCard(h, "open");
        CxDeviceSetCard(h, "init");
        std::cout << "[ModuleDAQ] 1073 slot=" << slot << " init OK\n";
    }

    if (card3010_) {
        CxDeviceSetCard(card3010_, "open");
        CxDeviceSetCard(card3010_, "init");
        std::cout << "[ModuleDAQ] 3010 init OK\n";
    }

    hwReady_ = true;
    return true;
}

// ── 硬件关闭（进程退出时）─────────────────────────────────────
bool ModuleDAQ::closeHW() {
    if (!hwReady_.exchange(false)) return false;

    if (card3010_) {
        CxDeviceSetCard(card3010_, "close");
        card3010_ = nullptr;
    }
    for (auto h : cards1073_)
        CxDeviceSetCard(h, "close");
    cards1073_.clear();

    CxDeviceSdkDeinit();
    std::cout << "[ModuleDAQ] HW closed\n";
    return true;
}

// ── 3010 寄存器控制 ─────────────────────────────────────────
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
