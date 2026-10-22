#include "config_editor_module.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <cerrno>

namespace msm {

using ordered_json = nlohmann::ordered_json;

namespace {
// ── 版本迁移 ─────────────────────────────────────────────
constexpr int kCurrentConfigVersion = 1;

// 配置默认值（仅迁移时补默认；不在文件中的键不动）
// 迁移函数：输入旧版本 cfg，输出补默认后的 cfg（只增量，不覆盖）
// 新版本新增字段时，追加迁移函数；对应 app_config 字段缺失时补默认。
void migrate_v1_to_v2(ordered_json& cfg) {
    // 示例：v1→v2 无新增字段；未来如新增字段在此补充
    // if (!cfg.contains("app_config")) cfg["app_config"] = ordered_json::object();
    // auto& ac = cfg["app_config"];
    // if (!ac.contains("SomeNewKey")) ac["SomeNewKey"] = "default";
    (void)cfg;
}

std::vector<std::pair<int, std::function<void(ordered_json&)>>> migrationChain() {
    return {
        {1, migrate_v1_to_v2},
    };
}

// ── 已知可编辑通道参数键（对齐 Qt ChannelConfig + DownSample）──
const std::vector<std::string>& editableKeysImpl() {
    static const std::vector<std::string> keys = {
        "DownSample", "TrigSource", "TrigMode", "TrigLength", "TrigWindowStart",
        "TrigLevel", "TrigPolarity", "TrigPulseWidth", "ExternalTrigPolarity",
        "TrigPosition", "TrigDelayTime", "FiniteTrigNum", "VoltageOffset",
        "VoltageRange", "DeadTime", "IpGlobalRST", "IpLogicRST", "WorkMode",
        "WaveDataEnable", "ValueThresholdHigh", "ValueThresholdLow",
        "K", "Kx", "Ky", "OffsetX", "OffsetY", "K1st", "B1st",
        "runEnable",
    };
    return keys;
}

// ── 枚举选项（文件值）──
const std::map<std::string, std::vector<std::string>>& enumsImpl() {
    static const std::map<std::string, std::vector<std::string>> opts = {
        {"DownSample", {"1GSa/s","500MSa/s","250MSa/s","125MSa/s","62.5MSa/s","31.25MSa/s"}},
        {"TrigSource", {"localChannel","localBoard","global","external","force","cycle"}},
        {"TrigMode", {"normal","autoMode"}},
        {"ExternalTrigPolarity", {"FallingE","RisingE"}},
        {"TrigPolarity", {"Falling","Rising"}},
        {"VoltageRange", {"1000","2000"}},
        {"WorkMode", {"vpp","vmin","vmax","sum_th_h","sum_th_l"}},
        {"WaveDataEnable", {"0","1"}},
        {"IpGlobalRST", {"0","1"}},
        {"IpLogicRST", {"0","1"}},
    };
    return opts;
}

const std::map<std::string, std::string>& cardTypeLabelsImpl() {
    static const std::map<std::string, std::string> labels = {
        {"1073", "数字化板卡(1073)"},
        {"3010", "时钟同步卡(3010)"},
    };
    return labels;
}

bool isKnownKey(const std::string& key) {
    for (auto& k : editableKeysImpl()) if (k == key) return true;
    return false;
}
// 判断某枚举键的值是否合法
static bool isEnumValid(const std::string& key, const std::string& value) {
    auto it = enumsImpl().find(key);
    if (it == enumsImpl().end()) return true;  // 非枚举键不校验
    for (auto& v : it->second) if (v == value) return true;
    return false;
}

// 身份键：编辑时不可覆盖
bool isIdentityKey(const std::string& key) {
    return key == "cardIndex" || key == "channel" || key == "slot";
}

std::string readFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return "";
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

} // namespace

// ── 构造 / 析构 ─────────────────────────────────────────
ConfigEditorModule::ConfigEditorModule() : msm::Module("ConfigEditor") {
    registerVars();
}

ConfigEditorModule::~ConfigEditorModule() {
    stop();
}

void ConfigEditorModule::registerVars() {
}

void ConfigEditorModule::start() {
    msm::Module::start();
}

void ConfigEditorModule::stop() {
    msm::Module::stop();
}

// ── import / export ──────────────────────────────────────
bool ConfigEditorModule::importConfig(const std::string& path, std::string* err) {
    std::lock_guard<std::mutex> lock(mtx_);
    std::string content = readFile(path);
    if (content.empty()) {
        if (err) *err = "cannot read config file: " + path;
        return false;
    }
    try {
        cfg_ = ordered_json::parse(content);
    } catch (const std::exception& e) {
        if (err) *err = std::string("JSON parse error: ") + e.what();
        return false;
    }
    loadedPath_ = path;
    // 版本迁移（含 .bak 备份 + 原子写回）
    if (!migrateIfNeeded()) {
        if (err) *err = "config migration failed";
        return false;
    }
    return true;
}

bool ConfigEditorModule::exportConfig(const std::string& path, std::string* err) {
    std::lock_guard<std::mutex> lock(mtx_);
    std::string target = path.empty() ? loadedPath_ : path;
    if (target.empty()) {
        if (err) *err = "no config path loaded";
        return false;
    }
    if (!saveToFileAtomic(target, cfg_.dump(4))) {
        if (err) *err = "write failed: " + target;
        return false;
    }
    loadedPath_ = target;
    return true;
}

// ── 版本迁移 ────────────────────────────────────────────
bool ConfigEditorModule::migrateIfNeeded() {
    if (cfg_.is_null()) return false;
    int ver = cfg_.value("ConfigVersion", 1);  // 旧文件无键 → v1
    if (ver >= kCurrentConfigVersion) return true;

    // 备份原文件
    std::string bak = loadedPath_ + ".bak";
    std::string content = readFile(loadedPath_);
    if (!content.empty()) {
        std::ofstream ofs(bak, std::ios::binary);
        if (ofs.is_open()) {
            ofs << content;
            ofs.flush();
            int bfd = ::open(bak.c_str(), O_RDONLY);
            if (bfd >= 0) { ::fsync(bfd); ::close(bfd); }
        }
    }

    // 依序迁移
    auto chain = migrationChain();
    for (auto& [fromVer, fn] : chain) {
        if (ver == fromVer) {
            fn(cfg_);
            ver++;
        }
    }
    // 写回新版本号
    cfg_["ConfigVersion"] = kCurrentConfigVersion;
    // 原子写回
    return saveToFileAtomic(loadedPath_, cfg_.dump(4));
}

// ── 原子写 ──────────────────────────────────────────────
bool ConfigEditorModule::saveToFileAtomic(const std::string& path, const std::string& content) {
    std::string tmp = path + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream ofs(tmp, std::ios::binary);
        if (!ofs.is_open()) return false;
        ofs << content;
        ofs.flush();
        int fd = ::open(tmp.c_str(), O_RDONLY);
        if (fd >= 0) { ::fsync(fd); ::close(fd); }
        else { ofs.close(); ::unlink(tmp.c_str()); return false; }
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

// ── 查询 ────────────────────────────────────────────────
std::string ConfigEditorModule::getConfigJson() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return cfg_.dump(4);
}

std::string ConfigEditorModule::currentVersion() const {
    std::lock_guard<std::mutex> lock(mtx_);
    int ver = cfg_.value("ConfigVersion", 1);
    return "v" + std::to_string(ver);
}

// ── 编辑语义 ────────────────────────────────────────────
ordered_json& ConfigEditorModule::configJson() {
    if (cfg_.is_null()) cfg_ = ordered_json::object();
    return cfg_;
}

ordered_json& ConfigEditorModule::cardsJson() {
    auto& cfg = configJson();
    if (!cfg.contains("DaqParameter")) cfg["DaqParameter"] = ordered_json::object();
    auto& dp = cfg["DaqParameter"];
    if (!dp.contains("Cards")) dp["Cards"] = ordered_json::object();
    return dp["Cards"];
}

ordered_json& ConfigEditorModule::appConfigJson() {
    auto& cfg = configJson();
    if (!cfg.contains("app_config")) cfg["app_config"] = ordered_json::object();
    return cfg["app_config"];
}

ordered_json* ConfigEditorModule::findCard(int cardIndex) {
    auto& cards = cardsJson();
    for (auto& [name, card] : cards.items()) {
        if (card.contains("CardIndex") && card["CardIndex"].is_number_integer() &&
            card["CardIndex"].get<int>() == cardIndex)
            return &card;
    }
    return nullptr;
}

std::string ConfigEditorModule::applyEdit(const std::string& scope, int cardIndex, int channel,
                                          const std::map<std::string, std::string>& params) {
    std::lock_guard<std::mutex> lock(mtx_);

    // 校验 params：键必须已知，枚举值必须合法，身份键禁止
    for (auto& [k, v] : params) {
        if (isIdentityKey(k)) return "error:identity_key:" + k;
        if (!isKnownKey(k)) return "error:unknown_key:" + k;
        if (!isEnumValid(k, v)) return "error:bad_enum:" + k;
    }

    ordered_json* card = nullptr;
    if (scope == "channel" || scope == "card") {
        card = findCard(cardIndex);
        if (!card) return "error:no_card:" + std::to_string(cardIndex);
    }

    // 应用写入
    auto writeToChannel = [&](ordered_json& ch) {
        for (auto& [k, v] : params) ch[k] = v;
    };

    if (scope == "channel") {
        std::string key = "Channel#" + std::to_string(channel);
        if (!(*card).contains("Channels") || !(*card)["Channels"].contains(key))
            return "error:no_channel:" + key;
        writeToChannel((*card)["Channels"][key]);
    } else if (scope == "card") {
        if (!(*card).contains("Channels") || (*card)["Channels"].is_null())
            return "error:no_channels";
        for (auto& [key, ch] : (*card)["Channels"].items())
            writeToChannel(ch);
    } else if (scope == "chassis") {
        auto& cards = cardsJson();
        for (auto& [cname, c] : cards.items()) {
            int idx = c.contains("CardIndex") && c["CardIndex"].is_number_integer()
                        ? c["CardIndex"].get<int>() : 0;
            if (idx == 3010) continue;  // 跳过时钟同步卡
            if (!c.contains("Channels") || c["Channels"].is_null()) continue;
            for (auto& [key, ch] : c["Channels"].items())
                writeToChannel(ch);
        }
    } else {
        return "error:bad_scope:" + scope;
    }

    // 原子写回
    return saveToFileAtomic(loadedPath_, cfg_.dump(4)) ? "ok" : "error:write_failed";
}

// ── 卡片增删 ─────────────────────────────────────────────────
std::string ConfigEditorModule::applyCardAdd(int slot, const std::string& cardType,
                                             int cloneFromCardIndex) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (slot <= 0) return "error:bad_slot";
    if (cardType != "1073" && cardType != "3010") return "error:bad_card_type:" + cardType;

    auto& cards = cardsJson();
    // slot 冲突（任意卡）
    for (auto& [k, c] : cards.items()) {
        if (c.contains("Slot") && c["Slot"].is_number_integer() && c["Slot"].get<int>() == slot)
            return "error:slot_in_use:" + std::to_string(slot);
    }

    if (cardType == "3010") {
        for (auto& [k, c] : cards.items()) {
            if (c.value("CardType", "") == "3010") return "error:already_has_3010";
        }
        ordered_json c3010;
        c3010["CardIndex"] = 3010;
        c3010["CardType"] = "3010";
        c3010["Channels"] = nullptr;
        c3010["Slot"] = slot;
        cards["Card#3010"] = std::move(c3010);
        return saveToFileAtomic(loadedPath_, cfg_.dump(4)) ? "ok" : "error:write_failed";
    }

    // 1073：最多 4 张（16ch 上限）；新卡号 = 最小未用号
    int n1073 = 0;
    for (auto& [k, c] : cards.items())
        if (c.value("CardType", "") == "1073") n1073++;
    if (n1073 >= 4) return "error:max_4_cards";
    int newIdx = 1;
    while (cards.contains("Card#" + std::to_string(newIdx))) newIdx++;

    ordered_json* src = cloneFromCardIndex > 0 ? findCard(cloneFromCardIndex) : nullptr;
    if (!src) {
        // 未指定克隆源 → 取第一张已有 1073
        for (auto& [k, c] : cards.items()) {
            if (c.value("CardType", "") == "1073") { src = &c; break; }
        }
    }
    if (!src || src->value("CardType", "") != "1073"
        || !src->contains("Channels") || (*src)["Channels"].is_null())
        return "error:no_source_card";

    ordered_json c;
    c["CardIndex"] = newIdx;
    c["CardType"] = "1073";
    c["Channels"] = (*src)["Channels"];   // 通道参数深拷贝
    c["Slot"] = slot;

    // 插入到数字顺序位置、3010 之前（通道编号跟随 JSON Card#N 顺序）
    ordered_json nc;
    bool inserted = false;
    for (auto& [k, v] : cards.items()) {
        int idx = v.contains("CardIndex") && v["CardIndex"].is_number_integer()
                    ? v["CardIndex"].get<int>() : 0;
        if (!inserted && (idx == 3010 || idx > newIdx)) {
            nc["Card#" + std::to_string(newIdx)] = c;
            inserted = true;
        }
        nc[k] = v;
    }
    if (!inserted) nc["Card#" + std::to_string(newIdx)] = c;
    cfg_["DaqParameter"]["Cards"] = std::move(nc);

    return saveToFileAtomic(loadedPath_, cfg_.dump(4)) ? "ok" : "error:write_failed";
}

std::string ConfigEditorModule::applyCardRemove(int cardIndex) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto& cards = cardsJson();
    std::string name;
    for (auto& [k, c] : cards.items()) {
        if (c.contains("CardIndex") && c["CardIndex"].is_number_integer()
            && c["CardIndex"].get<int>() == cardIndex) { name = k; break; }
    }
    if (name.empty()) return "error:no_card:" + std::to_string(cardIndex);
    ordered_json nc;
    for (auto& [k, v] : cards.items())
        if (k != name) nc[k] = v;
    cfg_["DaqParameter"]["Cards"] = std::move(nc);
    return saveToFileAtomic(loadedPath_, cfg_.dump(4)) ? "ok" : "error:write_failed";
}

// 合并 app_config（含类型保护）；成功返回空串，失败返回 "error:..."
std::string ConfigEditorModule::mergeAppConfig(ordered_json& app, ordered_json& ac) {
    for (auto& [k, v] : ac.items()) {
        // 类型保护：原值为数字/布尔，新值为字符串时尝试转换；转换失败则拒绝整次保存
        if (app.contains(k) && v.is_string()) {
            auto& old = app[k];
            if (old.is_boolean()) {
                std::string s = v.get<std::string>();
                if (s == "true") v = true;
                else if (s == "false") v = false;
                else return "error:type_mismatch:" + k;
            } else if (old.is_number()) {
                std::string s = v.get<std::string>();
                char* end = nullptr;
                double d = std::strtod(s.c_str(), &end);
                if (end && *end == '\0' && !s.empty()) v = d;
                else return "error:type_mismatch:" + k;
            }
        }
        app[k] = v;
    }
    return "";
}

std::string ConfigEditorModule::applyAppEdit(const std::string& appJsonStr) {
    std::lock_guard<std::mutex> lock(mtx_);
    ordered_json ac;
    try {
        ac = ordered_json::parse(appJsonStr);
    } catch (const std::exception& e) {
        return std::string("error:parse:") + e.what();
    }
    auto& app = appConfigJson();
    std::string err = mergeAppConfig(app, ac);
    if (!err.empty()) return err;
    return saveToFileAtomic(loadedPath_, cfg_.dump(4)) ? "ok" : "error:write_failed";
}

std::string ConfigEditorModule::applyAppEditToFile(const std::string& appJsonStr,
                                                   const std::string& targetFile) {
    std::lock_guard<std::mutex> lock(mtx_);
    ordered_json ac;
    try {
        ac = ordered_json::parse(appJsonStr);
    } catch (const std::exception& e) {
        return std::string("error:parse:") + e.what();
    }
    // 读取目标文件（若存在），合并 app_config 后原子写回
    ordered_json target;
    std::string content = readFile(targetFile);
    bool exists = !content.empty();
    if (exists) {
        try {
            target = ordered_json::parse(content);
        } catch (const std::exception&) {
            target = ordered_json::object();
            exists = false;
        }
    }
    if (!target.contains("app_config")) target["app_config"] = ordered_json::object();
    std::string err = mergeAppConfig(target["app_config"], ac);
    if (!err.empty()) return err;
    return saveToFileAtomic(targetFile, target.dump(4)) ? "ok" : "error:write_failed";
}

// ── 消息驱动 ────────────────────────────────────────────
void ConfigEditorModule::handleState(const msm::Message& msg) {
    auto sendResp = [&](const std::string& cmd) {
        msm::Message resp("Response");
        resp.setTarget(msg.getSender());
        resp.setCorrelationId(msg.getCorrelationId());
        resp.data.set("ok", cmd);
        msm::MessageBus::instance().send(resp);
    };

    if (msg.cmd == "GetConfig") {
        msm::Message resp("Response");
        resp.setTarget(msg.getSender());
        resp.setCorrelationId(msg.getCorrelationId());
        resp.data.set("config_json", getConfigJson());
        msm::MessageBus::instance().send(resp);
    } else if (msg.cmd == "SaveChannel" || msg.cmd == "SaveCard" || msg.cmd == "SaveChassis") {
        std::string scope = msg.cmd == "SaveChannel" ? "channel"
                          : msg.cmd == "SaveCard" ? "card" : "chassis";
        int cardIndex = msg.data.getInt("cardIndex", 0);
        int channel = msg.data.getInt("channel", 0);
        std::map<std::string, std::string> params;
        for (auto& [k, v] : msg.data.data) {
            if (k.rfind("param.", 0) == 0) params[k.substr(6)] = v;
        }
        std::string status = applyEdit(scope, cardIndex, channel, params);
        msm::Message resp("Response");
        resp.setTarget(msg.getSender());
        resp.setCorrelationId(msg.getCorrelationId());
        resp.data.set("status", status);
        msm::MessageBus::instance().send(resp);
    } else if (msg.cmd == "AddCard") {
        // WebUI /api/config/save scope=add-card：param.slot / param.cardType / param.cloneFrom
        std::map<std::string, std::string> params;
        for (auto& [k, v] : msg.data.data)
            if (k.rfind("param.", 0) == 0) params[k.substr(6)] = v;
        auto getParam = [&](const char* k, const std::string& dflt) {
            auto it = params.find(k);
            return it == params.end() ? dflt : it->second;
        };
        int slot = 0, cloneFrom = 0;
        try { slot = std::stoi(getParam("slot", "0")); } catch (...) {}
        try { cloneFrom = std::stoi(getParam("cloneFrom", "0")); } catch (...) {}
        std::string status = applyCardAdd(slot, getParam("cardType", "1073"), cloneFrom);
        msm::Message resp("Response");
        resp.setTarget(msg.getSender());
        resp.setCorrelationId(msg.getCorrelationId());
        resp.data.set("status", status);
        msm::MessageBus::instance().send(resp);
    } else if (msg.cmd == "RemoveCard") {
        std::string status = applyCardRemove(msg.data.getInt("cardIndex", 0));
        msm::Message resp("Response");
        resp.setTarget(msg.getSender());
        resp.setCorrelationId(msg.getCorrelationId());
        resp.data.set("status", status);
        msm::MessageBus::instance().send(resp);
    } else if (msg.cmd == "SaveApp") {
        std::string appJson = msg.data.get("app_config_json");
        std::string status = applyAppEdit(appJson);
        msm::Message resp("Response");
        resp.setTarget(msg.getSender());
        resp.setCorrelationId(msg.getCorrelationId());
        resp.data.set("status", status);
        msm::MessageBus::instance().send(resp);
    } else {
        // 未知命令：静默（不响应）
    }
}

// ── 静态元数据访问 ──────────────────────────────────────
const std::vector<std::string>& ConfigEditorModule::editableChannelKeys() {
    return editableKeysImpl();
}
const std::map<std::string, std::vector<std::string>>& ConfigEditorModule::enumOptions() {
    return enumsImpl();
}
const std::map<std::string, std::string>& ConfigEditorModule::cardTypeLabels() {
    return cardTypeLabelsImpl();
}

} // namespace msm
