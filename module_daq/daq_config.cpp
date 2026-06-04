#include "daq_config.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdlib>

// ============================================================
// 极简 JSON 解析工具（仅处理本项目配置格式）
// ============================================================
namespace {

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

// 在 json 中查找 key 对应的值（字符串或数字），不递归
static std::string findValue(const std::string& json, const std::string& key) {
    size_t p = json.find("\"" + key + "\"");
    if (p == std::string::npos) return "";
    p = json.find(':', p + key.size() + 2);
    if (p == std::string::npos) return "";
    p = json.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos) return "";
    if (json[p] == '"') {
        size_t e = json.find('"', p + 1);
        return (e == std::string::npos) ? "" : json.substr(p + 1, e - p - 1);
    }
    if (json[p] == 'n' && json.substr(p, 4) == "null") return "";
    size_t e = json.find_first_of(",}\n\r", p);
    return (e == std::string::npos) ? json.substr(p) : trim(json.substr(p, e - p));
}

// 在 json 中查找 key 对应的数值（int）
static int findInt(const std::string& json, const std::string& key, int def = 0) {
    std::string v = findValue(json, key);
    return v.empty() ? def : std::stoi(v);
}

// 查找 key 对应的花括号段 { ... }，返回内容
static std::string findBrace(const std::string& json, const std::string& key) {
    size_t p = json.find("\"" + key + "\"");
    if (p == std::string::npos) return "";
    p = json.find(':', p + key.size() + 2);
    if (p == std::string::npos) return "";
    p = json.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || json[p] != '{') return "";

    int depth = 1;
    size_t start = p;
    for (size_t i = p + 1; i < json.size(); i++) {
        if (json[i] == '{') depth++;
        else if (json[i] == '}') { depth--; if (depth == 0) return json.substr(start, i - start + 1); }
        else if (json[i] == '"') { i++; while (i < json.size() && json[i] != '"') i++; }
    }
    return "";
}

// 拆分 keyed 对象为子段列表，如 Card#1, Card#2, ...
// 输入: "{ \"Card#1\": { ... }, \"Card#2\": { ... } }"
// 输出: vector of "{ ... }" 内容
static std::vector<std::string> splitKeyedObjects(const std::string& brace_content) {
    std::vector<std::string> items;
    if (brace_content.empty() || brace_content == "null") return items;

    size_t i = 1; // skip '{'
    while (i < brace_content.size()) {
        // skip whitespace/comma
        i = brace_content.find_first_not_of(" \t\r\n,", i);
        if (i == std::string::npos || brace_content[i] == '}') break;

        // skip key string
        if (brace_content[i] == '"') {
            i = brace_content.find('"', i + 1);
            if (i == std::string::npos) break;
            i++;
        }
        i = brace_content.find(':', i);
        if (i == std::string::npos) break;
        i = brace_content.find_first_not_of(" \t\r\n", i + 1);
        if (i == std::string::npos) break;

        if (brace_content[i] == '{') {
            // extract brace object
            int depth = 1;
            size_t start = i;
            i++;
            while (i < brace_content.size() && depth > 0) {
                if (brace_content[i] == '{') depth++;
                else if (brace_content[i] == '}') depth--;
                else if (brace_content[i] == '"') { i++; while (i < brace_content.size() && brace_content[i] != '"') i++; }
                i++;
            }
            items.push_back(brace_content.substr(start, i - start));
        } else if (brace_content[i] == 'n' && brace_content.substr(i, 4) == "null") {
            i += 4;
        } else {
            // skip until comma or }
            i = brace_content.find_first_of(",}", i);
            if (i == std::string::npos) break;
        }
    }
    return items;
}

} // anonymous namespace

// ============================================================
// DaqConfig::loadFromJson
// ============================================================
bool DaqConfig::loadFromJson(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[DaqConfig] Cannot open " << path << "\n";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string json = ss.str();

    // --- app_config ---
    std::string app = findBrace(json, "app_config");
    if (!app.empty()) {
        auto_acq_on_start = findValue(app, "autoAcqInEpicsWhenSoftwareStarts") == "true";
        data_save_path = findValue(app, "dataSavePath");
        if (data_save_path.empty()) data_save_path = "/mnt/localssd";
        file_name = findValue(app, "fileName");
        file_type = findValue(app, "fileType");
        epics_control_mode = findValue(app, "isEpicsControlMode") == "true";
        max_save_size_mb = findInt(app, "maxSaveSizeMB", 1024);
        pv_info_freq_hz = findInt(app, "pvInfoUpdateFreHz", 2);
        run_id = findInt(app, "runId", 234);
        start_storage = findValue(app, "startStorage") == "true";
    }

    // --- DaqParameter ---
    std::string daq = findBrace(json, "DaqParameter");
    if (daq.empty()) {
        std::cerr << "[DaqConfig] No DaqParameter section\n";
        return false;
    }

    chassis_no = findInt(daq, "ChassisNo", 1);

    // --- DaqParameter.Cards ---
    std::string cards_brace = findBrace(daq, "Cards");
    if (cards_brace.empty()) {
        std::cerr << "[DaqConfig] No Cards section\n";
        return false;
    }

    auto card_blocks = splitKeyedObjects(cards_brace);
    for (const auto& cb : card_blocks) {
        CardConfig card;
        card.card_index = findInt(cb, "CardIndex");
        card.card_type = findValue(cb, "CardType");
        card.slot = findInt(cb, "Slot");

        // --- Card.Channels ---
        std::string ch_brace = findBrace(cb, "Channels");
        if (!ch_brace.empty() && ch_brace != "null") {
            auto ch_blocks = splitKeyedObjects(ch_brace);
            for (const auto& chb : ch_blocks) {
                ChannelConfig ch;
                ch.runEnable = findValue(chb, "runEnable") == "true" || findInt(chb, "runEnable", 1) != 0;
                ch.channel = findInt(chb, "channel", 1);
                ch.cardIndex = findInt(chb, "cardIndex", card.card_index);
                ch.slot = findInt(chb, "slot", card.slot);

                ch.trig_source = findValue(chb, "TrigSource");
                if (ch.trig_source.empty()) ch.trig_source = "global";
                ch.trig_mode = findValue(chb, "TrigMode");
                ch.trig_length = findValue(chb, "TrigLength");
                ch.trig_level = findValue(chb, "TrigLevel");
                ch.trig_pulse_width = findValue(chb, "TrigPulseWidth");
                ch.trig_polarity = findValue(chb, "TrigPolarity");
                ch.trig_delay_time = findValue(chb, "TrigDelayTime");
                ch.trig_position = findValue(chb, "TrigPosition");
                ch.trig_window_start = findValue(chb, "TrigWindowStart");

                ch.voltage_range = findValue(chb, "VoltageRange");
                ch.voltage_offset = findValue(chb, "VoltageOffset");

                ch.dead_time = findValue(chb, "DeadTime");
                ch.finite_trig_num = findValue(chb, "FiniteTrigNum");
                ch.wave_data_enable = findValue(chb, "WaveDataEnable");
                ch.work_mode = findValue(chb, "WorkMode");

                ch.k = findValue(chb, "K");
                ch.k1st = findValue(chb, "K1st");
                ch.kx = findValue(chb, "Kx");
                ch.ky = findValue(chb, "Ky");
                ch.offset_x = findValue(chb, "OffsetX");
                ch.offset_y = findValue(chb, "OffsetY");
                ch.b1st = findValue(chb, "B1st");

                ch.value_threshold_high = findValue(chb, "ValueThresholdHigh");
                ch.value_threshold_low = findValue(chb, "ValueThresholdLow");
                ch.external_trig_polarity = findValue(chb, "ExternalTrigPolarity");

                ch.ip_global_rst = findValue(chb, "IpGlobalRST");
                ch.ip_logic_rst = findValue(chb, "IpLogicRST");

                card.channels.push_back(ch);
            }
        }

        cards.push_back(card);
    }

    std::cout << "[DaqConfig] loaded " << cards.size() << " card(s) from " << path << "\n";
    for (const auto& c : cards) {
        std::cout << "  Card#" << c.card_index << " type=" << c.card_type
                  << " slot=" << c.slot
                  << " channels=" << c.channels.size() << "\n";
    }
    return true;
}

int DaqConfig::findCardByType(const std::string& card_type) const {
    for (int i = 0; i < (int)cards.size(); i++)
        if (cards[i].card_type == card_type) return i;
    return -1;
}

int DaqConfig::findCardByIndex(int card_index) const {
    for (int i = 0; i < (int)cards.size(); i++)
        if (cards[i].card_index == card_index) return i;
    return -1;
}

const ChannelConfig* DaqConfig::getChannel(int card_index, int channel) const {
    int ci = findCardByIndex(card_index);
    if (ci < 0) return nullptr;
    for (const auto& ch : cards[ci].channels)
        if (ch.channel == channel) return &ch;
    return nullptr;
}

ChannelConfig* DaqConfig::getChannel(int card_index, int channel) {
    int ci = findCardByIndex(card_index);
    if (ci < 0) return nullptr;
    for (auto& ch : cards[ci].channels)
        if (ch.channel == channel) return &ch;
    return nullptr;
}
