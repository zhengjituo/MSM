#include "config_editor_module.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
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
    vars_.declare("acquiring", "bool",
        [this]() { return acquiring_.load() ? "true" : "false"; });
}

void ConfigEditorModule::start() {
    msm::Module::start();
    running_.store(true);
    probeThread_ = std::thread([this] { probeLoop(); });
}

void ConfigEditorModule::stop() {
    if (!running_.exchange(false)) {
        msm::Module::stop();
        return;
    }
    if (probeThread_.joinable()) probeThread_.join();
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

std::string ConfigEditorModule::getAcquiringJson() const {
    return std::string("{\"acquiring\":") + (acquiring_.load() ? "true" : "false") + "}";
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
    if (acquiring_.load()) return "locked";

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

std::string ConfigEditorModule::applyAppEdit(const std::string& appJsonStr) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (acquiring_.load()) return "locked";
    ordered_json ac;
    try {
        ac = ordered_json::parse(appJsonStr);
    } catch (const std::exception& e) {
        return std::string("error:parse:") + e.what();
    }
    auto& app = appConfigJson();
    for (auto& [k, v] : ac.items()) app[k] = v;
    return saveToFileAtomic(loadedPath_, cfg_.dump(4)) ? "ok" : "error:write_failed";
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
    } else if (msg.cmd == "GetLock") {
        msm::Message resp("Response");
        resp.setTarget(msg.getSender());
        resp.setCorrelationId(msg.getCorrelationId());
        resp.data.set("acquiring", acquiring_.load() ? "true" : "false");
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

// ── DAQ 门控 ────────────────────────────────────────────
bool ConfigEditorModule::probeDaqRunning() {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, "/tmp/msm_daq.sock", sizeof(addr.sun_path) - 1);
    if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }
    const char* req = "MSG Status >> -@ DAQModule\n";
    ::send(fd, req, std::strlen(req), MSG_NOSIGNAL);
    struct pollfd pfd{fd, POLLIN, 0};
    int n = ::poll(&pfd, 1, 1500);
    bool running = false;
    if (n > 0 && (pfd.revents & POLLIN)) {
        char buf[2048];
        int r = ::recv(fd, buf, sizeof(buf) - 1, 0);
        if (r > 0) {
            buf[r] = 0;
            std::string resp(buf);
            // 状态响应中是否包含 running 状态
            if (resp.find("\"running\":true") != std::string::npos ||
                resp.find("running=true") != std::string::npos ||
                resp.find("running:true") != std::string::npos)
                running = true;
        }
    }
    ::close(fd);
    return running;
}

void ConfigEditorModule::probeLoop() {
    while (running_.load()) {
        acquiring_.store(probeDaqRunning());
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

} // namespace msm
