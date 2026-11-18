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
#include <unordered_map>
#include <set>
#include <ctime>
#include <sys/time.h>

// 原子取 max（API 计时用）
static void atomicMax(std::atomic<int64_t>& v, int64_t x) {
    for (;;) {
        int64_t cur = v.load(std::memory_order_relaxed);
        if (x <= cur) return;
        if (v.compare_exchange_weak(cur, x, std::memory_order_relaxed)) return;
    }
}

// 启动/停止时序日志的墙钟戳（ms）：验证「板卡先采集、3010 触发后使能」的间隔
static std::string nowStamp() {
    struct timeval tv; gettimeofday(&tv, nullptr);
    struct tm lt; localtime_r(&tv.tv_sec, &lt);
    char b[16]; strftime(b, sizeof(b), "%H:%M:%S", &lt);
    return std::string(b) + "." + std::to_string(tv.tv_usec / 1000);
}

// 整型列表拼接（错误信息 / 状态摘要）
static std::string joinInts(const std::vector<int>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += ",";
        s += std::to_string(v[i]);
    }
    return s.empty() ? "无" : s;
}

// WorkMode string → int mapping (ref Qt KWorkModeMap)
static int mapWorkMode(const std::string& mode) {
    static const std::unordered_map<std::string, int> m = {
        {"vpp", 0}, {"vmin", 1}, {"vmax", 2},
        {"sum_th_h", 3}, {"sum_th_l", 4}
    };
    auto it = m.find(mode);
    return (it != m.end()) ? it->second : 0;
}

// TrigSource 字符串 → SDK 数值（SDK 只认数字，传名字会 StringToUint32 失败）
// 多卡时必须每张卡都是 global(4)：3010 的触发经背板广播给所有 1073，帧号才同步
static std::string mapTrigSource(const std::string& v) {
    static const std::unordered_map<std::string, std::string> m = {
        {"localChannel", "1"}, {"localBoard", "2"}, {"global", "4"},
        {"external", "8"}, {"force", "16"}, {"cycle", "32"}
    };
    auto it = m.find(v);
    return (it != m.end()) ? it->second : v;   // 数字原样透传
}

// TrigMode 配置名 → SDK 数值：厂商 demo 的 global 触发用例硬编码 "2"
//（demo 配置 JSON 里写 "normal"，但其 C++ 路径传的是数字）
static std::string mapTrigMode(const std::string& v) {
    if (v == "normal") return "2";
    return v;
}

// TrigPolarity 配置名 → SDK 数值（demo 用 "1"，对应 rising）
static std::string mapTrigPolarity(const std::string& v) {
    if (v == "Rising") return "1";
    if (v == "Falling") return "0";
    return v;
}


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
        s.slots.push_back(slot.slot);
    }
    return s;
}

void ModuleDAQ::onIdle() {
    // 帧读回由每卡专属读线程驱动（readLoop），消息循环只处理控制消息
}

void ModuleDAQ::handleState(const msm::Message& msg) {
    if (msg.cmd == "Init") {
        if (emergencyStop_.load()) emergencyStop_ = false;
        onReadConfig(msg.data);
        // 多卡严格匹配：hw 未就绪，或配置卡集合与已打开集合不一致 → 重新扫描/打开
        std::string err;
        bool ok = true;
        if (!hwReady_.load() || !cardSetMatchesOpen()) {
            ok = initHW(err);
        }
        if (ok && !updateConfig()) {
            ok = false;
            err = "无活动通道（检查配置 runEnable 与通道数）";
        }
        if (!ok)
            std::cerr << "[ModuleDAQ] Init failed: " << err << "\n";
        setInitFailed(!ok, ok ? "" : err);
        // 回 Response：start 链据 status 判断是否继续
        if (!msg.getSender().empty()) {
            msm::Message response("Response");
            response.setTarget(msg.getSender());
            response.setCorrelationId(msg.getCorrelationId());
            response.data.set("status", ok ? "ok" : "error");
            if (!ok) response.data.set("error", err);
            msm::MessageBus::instance().send(response);
        }
    } else if (msg.cmd == "Configure" || msg.cmd == "ReadConfig") {
        onReadConfig(msg.data);
        updateConfig();
    } else if (msg.cmd == "InitHW") {
        std::string err;
        if (!initHW(err))
            std::cerr << "[ModuleDAQ] InitHW failed: " << err << "\n";
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
    // 对照厂商 demo 的 global 触发用例补齐：GlobalTrigMode(0=或逻辑,1=与逻辑)、
    // ExternalTrigLevel。双卡 bunch 相位的二分实验已排除 GlobalTrigMode
    // （不设它相位仍同步），此处作为厂商参考值钉死
    for (auto h : cards1073_) {
        CxDeviceSetCardValue(h, "ClockSource", "0");
        CxDeviceSetCardValue(h, "SyncWindowEnable", "0");
        CxDeviceSetCardValue(h, "SyncSource", "0");
        CxDeviceSetCardValue(h, "GlobalTrigMode", "0");
        CxDeviceSetCardValue(h, "ExternalTrigLevel", "20");
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
            CxDeviceSetChannelValue(hCard, ch, "TrigSource", mapTrigSource(chCfg.trig_source).c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigMode", mapTrigMode(chCfg.trig_mode).c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigLength", chCfg.trig_length.c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigPulseWidth", chCfg.trig_pulse_width.c_str());
            CxDeviceSetChannelValue(hCard, ch, "VoltageRange", chCfg.voltage_range.c_str());
            CxDeviceSetChannelValue(hCard, ch, "VoltageOffset", chCfg.voltage_offset.c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigLevel", chCfg.trig_level.c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigPolarity", mapTrigPolarity(chCfg.trig_polarity).c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigPosition", chCfg.trig_position.c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigWindowStart", chCfg.trig_window_start.c_str());
            CxDeviceSetChannelValue(hCard, ch, "TrigDelayTime",
                std::to_string(config_.trig_delay_time_fix +
                    (chCfg.trig_delay_time.empty() ? 0 : std::stoi(chCfg.trig_delay_time))).c_str());
            CxDeviceSetChannelValue(hCard, ch, "ExternalTrigPolarity", chCfg.external_trig_polarity.c_str());
            CxDeviceSetChannelValue(hCard, ch, "DeadTime", chCfg.dead_time.c_str());
            CxDeviceSetChannelValue(hCard, ch, "FiniteTrigNum", chCfg.finite_trig_num.c_str());
            CxDeviceSetChannelValue(hCard, ch, "WorkMode", std::to_string(mapWorkMode(chCfg.work_mode)).c_str());
            CxDeviceSetChannelValue(hCard, ch, "WaveDataEnable", chCfg.wave_data_enable.c_str());
            CxDeviceSetChannelValue(hCard, ch, "ValueThresholdHigh", chCfg.value_threshold_high.c_str());
            CxDeviceSetChannelValue(hCard, ch, "ValueThresholdLow", chCfg.value_threshold_low.c_str());
            CxDeviceSetChannelValue(hCard, ch, "IpGlobalRST", chCfg.ip_global_rst.c_str());
            CxDeviceSetChannelValue(hCard, ch, "IpLogicRST", chCfg.ip_logic_rst.c_str());
            if (!chCfg.k1st.empty())
                CxDeviceSetChannelValue(hCard, ch, "EffectiveDataExtractionK1st",
                    chCfg.k1st.c_str());
            if (!chCfg.b1st.empty())
                CxDeviceSetChannelValue(hCard, ch, "EffectiveDataExtractionB1st",
                    chCfg.b1st.c_str());
            // Kx/Ky/K/OffsetX/OffsetY 仅通道1
            if (ch == 1) {
                if (!chCfg.kx.empty())
                    CxDeviceSetChannelValue(hCard, ch, "EffectiveDataExtractionKX",
                        chCfg.kx.c_str());
                if (!chCfg.ky.empty())
                    CxDeviceSetChannelValue(hCard, ch, "EffectiveDataExtractionKY",
                        chCfg.ky.c_str());
                if (!chCfg.k.empty())
                    CxDeviceSetChannelValue(hCard, ch, "EffectiveDataExtractionK",
                        chCfg.k.c_str());
                if (!chCfg.offset_x.empty())
                    CxDeviceSetChannelValue(hCard, ch, "EffectiveDataExtractionOffsetX",
                        chCfg.offset_x.c_str());
                if (!chCfg.offset_y.empty())
                    CxDeviceSetChannelValue(hCard, ch, "EffectiveDataExtractionOffsetY",
                        chCfg.offset_y.c_str());
            }
            std::cout << "[ModuleDAQ]   ch" << ch << " configured\n";

            // ring 数量由 main 按进程启动时的配置创建，不支持运行中扩卡；
            // 越界会变成野指针写崩，这里明确报错
            if (activeChCount_ >= (int)rings_.size()) {
                std::cerr << "[ModuleDAQ] ERROR: ring 数量不足（"
                          << activeChCount_ + 1 << "/" << rings_.size()
                          << "）：启用通道数超过进程启动时的通道数，"
                          "卡集合变更后请重启进程\n";
                return false;
            }
            auto& slot = activeChannels_[activeChCount_++];
            slot.cardHandle = hCard;
            slot.ch = ch;
            slot.slot = cardCfg.slot;
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

    // 先关掉 acquiring_，避免启动过程中读线程把旧数据写进 ring
    acquiring_ = false;

    // ── 启动接收 ────────────────────────────────────────────
    for (int i = 0; i < activeChCount_; i++) {
        auto& slot = activeChannels_[i];
        // 32MB ≈ 280ms 数据：吸收读线程偶发停顿（实测 spike 至 150ms），
        // 16MB(139ms) 在双卡下不足以覆盖 pass 周期
        CxDeviceSetChannelRecvBufferSize(slot.cardHandle, slot.ch, 32 * 1024 * 1024);
        CxDeviceStartChannelRecv(slot.cardHandle, slot.ch);
        CxDeviceSetChannel(slot.cardHandle, slot.ch, "run");
        std::cout << "[ModuleDAQ] [" << nowStamp() << "] ch" << slot.ch << " (id=" << i << ") recv started\n";
    }

    // ── 3010 trigger ON（等所有通道启动稳定再开触发） ────
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    setTriggerOutput(true);
    std::cout << "[ModuleDAQ] [" << nowStamp() << "] 3010 trigger enabled\n";

    emergencyStop_ = false;
    acquiring_ = true;
    for (int i = 0; i < activeChCount_; i++) {
        activeChannels_[i].totalFrames.store(0);
        activeChannels_[i].apiCalls.store(0);
        activeChannels_[i].apiUs.store(0);
        activeChannels_[i].apiMaxUs.store(0);
        activeChannels_[i].copyUs.store(0);
        activeChannels_[i].copyMaxUs.store(0);
        activeChannels_[i].contigChecked = false;
        activeChannels_[i].contigOk = true;
    }
    std::cout << "[ModuleDAQ] Acquiring (per-card read threads)\n";

    // DAQ ready → start Process consumer from clean ring state
    msm::Message startProcess("Start");
    startProcess.setTarget("Process");
    msm::MessageBus::instance().send(startProcess);

    // 启动每卡读线程（在 Process 启动后，ring 从干净状态开始）
    startReadThreads();

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

    // 等读线程退出（避免与 stopChannels/flushAll 并发调 SDK 同一卡句柄）
    joinReadThreads();

    setTriggerOutput(false);
    std::cout << "[ModuleDAQ] [" << nowStamp() << "] 3010 trigger off\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    stopChannels();
    flushAll();
    std::cout << "[DAQ] stop:";
    for (int i = 0; i < activeChCount_; i++)
        std::cout << " ch" << activeChannels_[i].ch << "=" << activeChannels_[i].totalFrames.load();
    std::cout << std::endl;

    // API 读回耗时汇总（ms / 单次最大 µs）：验证双卡下 SDK 调用是否被阻塞
    std::cout << "[DAQ] read-time:";
    for (int i = 0; i < activeChCount_; i++) {
        auto& s = activeChannels_[i];
        std::cout << " ch" << s.ch << "{api=" << (s.apiUs.load() / 1000)
                  << "ms,max=" << s.apiMaxUs.load()
                  << "us,copy=" << (s.copyUs.load() / 1000)
                  << "ms,max=" << s.copyMaxUs.load() << "us}";
    }
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
        response.data.set("ch" + std::to_string(i + 1) + "_slot",
                          std::to_string(s.slots[i]));
    }
    response.data.set("running", s.is_running ? "true" : "false");
    // 最近一次机箱扫描结果（多卡配置/排障用）
    response.data.set("scanned_1073_slots", joinInts(scanned1073Slots_));
    response.data.set("scanned_3010_slot", std::to_string(scanned3010Slot_));
}

// 每张卡一个读线程：chIds 为该卡的通道下标。
// 背景：8 通道单线程串行读，每通道 SDK 调用+memcpy ~30ms → pass 周期 ~175ms
// 超过 SDK 16MB 缓冲的保持时间(~139ms) → 缓冲 wrap 丢帧（双卡实测 ~8%）。
// 拆成每卡一线程后周期 ~90ms < 139ms，且两卡并行。
void ModuleDAQ::readLoop(int cardNo, const std::vector<int>& chIds) {
    auto lastPass = std::chrono::steady_clock::now();
    int64_t passNo = 0;
    int slotNo = activeChannels_[chIds[0]].slot;

    while (acquiring_.load() && !emergencyStop_.load()) {
        auto passNow = std::chrono::steady_clock::now();
        passNo++;
        // pass 间隔探针：读线程停顿 >50ms 会打印（帧缺口归因：宿主 stall vs 上游丢帧）
        int64_t passGapMs = std::chrono::duration_cast<std::chrono::milliseconds>(passNow - lastPass).count();
        lastPass = passNow;
        int64_t passFrames = 0;
        bool any = false;

        for (int i : chIds) {
            if (emergencyStop_.load()) return;

            auto& slot = activeChannels_[i];
            void* frames = nullptr;
            uint32_t n = 0;
            auto t0 = std::chrono::steady_clock::now();
            CxDeviceReadChannelFrames(slot.cardHandle, slot.ch, &frames, &n);
            auto t1 = std::chrono::steady_clock::now();
            int64_t apiUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            slot.apiCalls.fetch_add(1, std::memory_order_relaxed);
            slot.apiUs.fetch_add(apiUs, std::memory_order_relaxed);
            atomicMax(slot.apiMaxUs, apiUs);
            if (n == 0) continue;
            passFrames += n;
            any = true;

            auto* rp = static_cast<RawPack*>(frames);
            size_t flen = rp[0].BufferSize;
            if (flen == 0) continue;
            size_t bytes = (size_t)n * flen;

            thread_local std::vector<uint8_t> buf;
            if (bytes > buf.size())
                buf.resize(bytes);

            // SDK 文档：RawPack[].Buffer 为连续 n 帧；首次验证，连续则单次大 memcpy
            if (!slot.contigChecked) {
                slot.contigChecked = true;
                for (uint32_t j = 1; j < n; j++)
                    if (rp[j].Buffer != rp[0].Buffer + (size_t)j * flen) {
                        slot.contigOk = false;
                        std::cerr << "[ModuleDAQ] ch" << slot.ch
                                  << " SDK 返回帧不连续，回退逐帧拷贝\n";
                        break;
                    }
            }
            if (slot.contigOk) {
                std::memcpy(buf.data(), rp[0].Buffer, bytes);
            } else {
                uint8_t* dst = buf.data();
                for (uint32_t j = 0; j < n; j++) {
                    if (!rp[j].Buffer) { bytes = 0; break; }
                    std::memcpy(dst, rp[j].Buffer, flen);
                    dst += flen;
                }
                if (bytes == 0) continue;
            }

            slot.frameSize.store(flen);
            slot.ring->setExpectedPacketSize(flen);
            size_t written = slot.ring->writeFromDevice(buf.data(), bytes);
            if (written < bytes) {
                // ring 满 → 本 pass 整批丢弃。节流告警（每通道 1 条/秒），
                // 丢帧必须可见：计数同时经 Status chN_overflow 暴露到 PV/webui
                int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (nowMs - slot.lastOvfLogMs > 1000) {
                    slot.lastOvfLogMs = nowMs;
                    std::cerr << "[ModuleDAQ] ch" << slot.ch << " OVERFLOW: ring 满，本 pass 丢 "
                              << (bytes - written) / flen << " 帧（累计 "
                              << slot.ring->getOverflowCount() << " 次丢弃）\n";
                }
            }
            auto t2 = std::chrono::steady_clock::now();
            int64_t copyUs = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
            slot.copyUs.fetch_add(copyUs, std::memory_order_relaxed);
            atomicMax(slot.copyMaxUs, copyUs);
            int64_t total = slot.totalFrames.fetch_add(written / flen) + (int64_t)(written / flen);

            // 读帧量在 Process/Storage 端打印，此处静默
        }

        if (!any)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));

        if (passGapMs > 50)
            std::cout << "[ModuleDAQ] readpass gap: " << passGapMs
                      << "ms (card" << cardNo << " slot=" << slotNo
                      << ", pass " << passNo << ", frames=" << passFrames << ")\n";
    }
}

void ModuleDAQ::startReadThreads() {
    joinReadThreads();
    std::unordered_map<void*, std::vector<int>> byCard;
    for (int i = 0; i < activeChCount_; i++)
        byCard[activeChannels_[i].cardHandle].push_back(i);
    int cardNo = 0;
    for (auto& kv : byCard) {
        const auto& ids = kv.second;
        std::cout << "[ModuleDAQ] read thread " << ++cardNo
                  << " started (card slot=" << activeChannels_[ids[0]].slot
                  << ", " << ids.size() << " ch)\n";
        readThreads_.emplace_back([this, cardNo, ids]() { readLoop(cardNo, ids); });
    }
}

void ModuleDAQ::joinReadThreads() {
    for (auto& t : readThreads_)
        if (t.joinable()) t.join();
    readThreads_.clear();
}

void ModuleDAQ::flushAll() {
    if (activeChCount_ == 0) return;
    std::vector<uint8_t> buf(4U * 1024U * 1024U);

    for (int i = 0; i < activeChCount_; i++) {
        auto& slot = activeChannels_[i];
        void* frames = nullptr;
        uint32_t n = 0;
        auto f0 = std::chrono::steady_clock::now();
        CxDeviceReadChannelFrames(slot.cardHandle, slot.ch, &frames, &n);
        int64_t fus = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - f0).count();
        std::cout << "[flush] ch" << slot.ch << " api=" << fus << "us n=" << n << std::endl;
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
        if (written < bytes)
            std::cerr << "[ModuleDAQ] ch" << slot.ch << " OVERFLOW(flush): ring 满，丢 "
                      << (bytes - written) / flen << " 帧\n";
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
// ── 配置期望的卡集合 ─────────────────────────────────────────
std::set<int> ModuleDAQ::expected1073Slots() const {
    std::set<int> slots;
    for (const auto& c : config_.cards)
        if (c.card_type == "1073") slots.insert(c.slot);
    return slots;
}

int ModuleDAQ::expected3010Slot() const {
    for (const auto& c : config_.cards)
        if (c.card_type == "3010") return c.slot;
    return -1;
}

int ModuleDAQ::expected3010Count() const {
    int n = 0;
    for (const auto& c : config_.cards)
        if (c.card_type == "3010") n++;
    return n;
}

bool ModuleDAQ::cardSetMatchesOpen() const {
    if (!hwReady_.load()) return false;
    std::set<int> open;
    for (auto h : cards1073_) {
        uint32_t slot = 0;
        CxDeviceGetCardSlotNo(h, &slot);
        open.insert((int)slot);
    }
    int open3010 = -1;
    if (card3010_) {
        uint32_t slot = 0;
        CxDeviceGetCardSlotNo(card3010_, &slot);
        open3010 = (int)slot;
    }
    return open == expected1073Slots() && open3010 == expected3010Slot();
}

// ── 硬件初始化 / 重配（严格按配置 slot 打开）──────────────────
bool ModuleDAQ::initHW(std::string& error) {
    // 1) 先关闭已打开的卡（SDK 保持初始化）
    if (card3010_) {
        CxDeviceSetCard(card3010_, "close");
        card3010_ = nullptr;
    }
    for (auto h : cards1073_)
        CxDeviceSetCard(h, "close");
    cards1073_.clear();
    hwReady_ = false;
    activeChCount_ = 0;

    // 2) SDK 初始化（仅首次）
    bool sdkWasReady = sdkReady_;
    if (!sdkReady_) {
        std::cout << "[ModuleDAQ] Initializing SDK...\n";
        CxDeviceSetSdkOption("WithLog", "false");
        if (CxDeviceSdkInit() != 0) {
            error = "SDK 初始化失败";
            return false;
        }
        sdkReady_ = true;
    }

    // 3) 扫描机箱
    CxChassisHandle chassis = nullptr;
    CxCardHandle* hwCards = nullptr;
    uint32_t hwCount = 0;
    CxDeviceGetChassis(PXIe, &chassis);
    CxDeviceGetCards(chassis, &hwCards, &hwCount);
    std::cout << "[ModuleDAQ] Found " << hwCount << " cards in chassis\n";

    // 首次初始化：全卡 open → reset → close
    // 上次进程异常退出（崩溃/被 kill）会留下脏的触发/DMA 状态，
    // 表现为后续 Start 只收到一个接收缓冲的量甚至 0 帧
    if (!sdkWasReady) {
        for (uint32_t i = 0; i < hwCount; i++) {
            CxDeviceSetCard(hwCards[i], "open");
            CxDeviceResetCard(hwCards[i]);
            CxDeviceSetCard(hwCards[i], "close");
        }
        std::cout << "[ModuleDAQ] all " << hwCount << " card(s) reset (清残留状态)\n";
    }

    std::vector<std::pair<int, void*>> scanned1073;   // slot, handle
    void* scanned3010 = nullptr;
    int scanned3010Slot = -1;
    for (uint32_t i = 0; i < hwCount; i++) {
        uint32_t did = 0, slot = 0;
        CxDeviceGetCardDeviceId(hwCards[i], &did);
        CxDeviceGetCardSlotNo(hwCards[i], &slot);
        if (did == 0x1073) {
            scanned1073.push_back({(int)slot, hwCards[i]});
        } else if (did == 0x3010) {
            scanned3010 = hwCards[i];
            scanned3010Slot = (int)slot;
        }
    }
    std::sort(scanned1073.begin(), scanned1073.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    scanned1073Slots_.clear();
    for (auto& p : scanned1073) scanned1073Slots_.push_back(p.first);
    scanned3010Slot_ = scanned3010Slot;

    // 4) 配置期望集合校验（严格匹配，缺卡报错）
    const auto exp1073 = expected1073Slots();
    const int exp3010 = expected3010Slot();
    const int n3010 = expected3010Count();

    if (exp1073.empty()) {
        error = "配置中没有 1073 板卡（Cards 里需要 CardType=1073）";
        return false;
    }
    if ((int)exp1073.size() > 4) {
        error = "配置 1073 数量 " + std::to_string(exp1073.size()) +
                " 超过上限 4（最多 4 卡 × 4 通道）";
        return false;
    }
    if (n3010 > 1) {
        error = "配置中 3010 卡数量 " + std::to_string(n3010) + " 超过上限 1";
        return false;
    }

    std::set<int> scannedSet;
    for (auto& p : scanned1073) scannedSet.insert(p.first);
    std::vector<int> missing;
    for (int s : exp1073)
        if (!scannedSet.count(s)) missing.push_back(s);
    if (!missing.empty()) {
        error = "配置要求 1073 slot=" + joinInts(missing) +
                "，机箱未找到（实际 1073 slots: " + joinInts(scanned1073Slots_) +
                "，共 " + std::to_string(scanned1073.size()) + " 张）";
        return false;
    }
    if (exp3010 >= 0 && scanned3010Slot < 0) {
        error = "配置要求 3010 slot=" + std::to_string(exp3010) +
                "，机箱未找到（实际 3010: 无）";
        return false;
    }

    // 5) 只打开配置中的卡；机箱多余卡忽略
    for (auto& p : scanned1073) {
        int slot = p.first;
        if (!exp1073.count(slot)) {
            std::cout << "[ModuleDAQ] INFO: 忽略未配置的 1073 slot=" << slot << "\n";
            continue;
        }
        CxDeviceSetCard(p.second, "open");
        CxDeviceSetCard(p.second, "init");
        std::cout << "[ModuleDAQ] 1073 slot=" << slot << " init OK\n";
        cards1073_.push_back(p.second);
    }

    if (exp3010 >= 0) {
        CxDeviceSetCard(scanned3010, "open");
        CxDeviceSetCard(scanned3010, "init");
        card3010_ = scanned3010;
        std::cout << "[ModuleDAQ] 3010 slot=" << scanned3010Slot << " init OK\n";
    }

    std::cout << "[ModuleDAQ] " << cards1073_.size() << " x 1073, "
              << (card3010_ ? 1 : 0) << " x 3010 opened\n";
    hwReady_ = true;
    return true;
}

// ── 硬件关闭（进程退出时）─────────────────────────────────────
bool ModuleDAQ::closeHW() {
    if (!hwReady_.exchange(false) && !sdkReady_) return false;

    if (card3010_) {
        CxDeviceSetCard(card3010_, "close");
        card3010_ = nullptr;
    }
    for (auto h : cards1073_)
        CxDeviceSetCard(h, "close");
    cards1073_.clear();

    if (sdkReady_) {
        CxDeviceSdkDeinit();
        sdkReady_ = false;
    }
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
