// ── msm_cli_config ────────────────────────────────────────────
// 供 PyDM 通过 subprocess 调用的配置编辑 CLI（复用 ConfigEditorModule 文件 API）。
// 约定：
//   - stdout 只输出结果 JSON（PyDM 直接 json.loads(stdout)）；
//     交互/调试输出一律走 stderr。
//   - 不抛异常/不崩溃：错误以 {"status":"error:...","msg":"..."} 输出，退出码 1。
//
// 用法：
//   msm_cli_config meta     [--config PATH]
//   msm_cli_config get      [--config PATH]
//   msm_cli_config save     [--config PATH] --scope channel|card|chassis \
//                           --card N --channel M --set key=value [--set k2=v2 ...]
//   msm_cli_config save-app [--config PATH] --json '<app_config_json>'
//
// 退出码：0=ok   1=error（文件读不到 / JSON 解析失败 / 参数缺失等）  2=locked
#include "config_editor_module.h"
#include <nlohmann/json.hpp>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using nlohmann::ordered_json;
using msm::ConfigEditorModule;

// 编译期注入的项目根目录（与 main_simsrc 一致）
static const std::string kProjectRoot = MSM_PROJECT_ROOT;
static const std::string kDefaultConfig =
    kProjectRoot + "/config/app_pending_config.json";

// ── 输出约定：stdout 只放结果 JSON ──────────────────────────
// 返回值即进程退出码：ok→0，locked→2，其它→1
static int emitStatus(const std::string& status) {
    ordered_json j;
    j["status"] = status;
    std::cout << j.dump() << "\n";
    if (status == "ok") return 0;
    if (status == "locked") return 2;
    return 1;
}

static int emitError(const std::string& status, const std::string& msg) {
    ordered_json j;
    j["status"] = status;
    if (!msg.empty()) j["msg"] = msg;
    std::cout << j.dump() << "\n";
    return 1;
}

// 相对路径 → 绝对路径（解析失败时原样返回）
static std::string toAbsPath(const std::string& p) {
    char buf[4096];
    if (realpath(p.c_str(), buf) != nullptr) return std::string(buf);
    return p;
}

// ── 参数解析 ────────────────────────────────────────────────
struct CmdArgs {
    std::string config = kDefaultConfig;
    std::string scope;
    bool hasCard = false, hasChannel = false, hasJson = false;
    long card = 0, channel = 0;
    std::string json;
    std::map<std::string, std::string> params;   // --set key=value
};

static bool parseLong(const std::string& s, long& out, std::string& err) {
    if (s.empty()) { err = "empty integer"; return false; }
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') { err = "not an integer: " + s; return false; }
    out = v;
    return true;
}

// 按第一个 '=' 切分 key=value（值允许包含 '='）
static bool splitSet(const std::string& s, std::string& key, std::string& value,
                     std::string& err) {
    size_t pos = s.find('=');
    if (pos == std::string::npos || pos == 0) {
        err = "bad --set arg (expect key=value): " + s;
        return false;
    }
    key = s.substr(0, pos);
    value = s.substr(pos + 1);
    return true;
}

// 从 argv[2..] 解析选项；支持 "--opt value" 与 "--opt=value" 两种写法
static bool parseArgs(int argc, char** argv, CmdArgs& a, std::string& err) {
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--", 0) != 0) {
            err = "unexpected positional arg: " + arg;
            return false;
        }
        std::string name = arg, inlineVal;
        bool hasInline = false;
        size_t eq = arg.find('=');
        if (eq != std::string::npos) {
            name = arg.substr(0, eq);
            inlineVal = arg.substr(eq + 1);
            hasInline = true;
        }
        // 取该选项的值（行内或下一个 argv）
        auto needValue = [&](const char* what) -> bool {
            if (hasInline) return true;
            if (i + 1 >= argc) {
                err = std::string("missing value for ") + what;
                return false;
            }
            inlineVal = argv[++i];
            return true;
        };

        if (name == "--config") {
            if (!needValue("--config")) return false;
            a.config = inlineVal;
        } else if (name == "--scope") {
            if (!needValue("--scope")) return false;
            a.scope = inlineVal;
        } else if (name == "--card") {
            if (!needValue("--card")) return false;
            if (!parseLong(inlineVal, a.card, err)) return false;
            a.hasCard = true;
        } else if (name == "--channel" || name == "--ch") {
            if (!needValue("--channel")) return false;
            if (!parseLong(inlineVal, a.channel, err)) return false;
            a.hasChannel = true;
        } else if (name == "--json") {
            if (!needValue("--json")) return false;
            a.json = inlineVal;
            a.hasJson = true;
        } else if (name == "--set") {
            if (!needValue("--set")) return false;
            std::string k, v;
            if (!splitSet(inlineVal, k, v, err)) return false;
            a.params[k] = v;
        } else {
            err = "unknown option: " + name;
            return false;
        }
    }
    return true;
}

// ── meta：文件绝对路径 + 版本 + 可编辑键/枚举选项/卡类型标签 ─
static int cmdMeta(const std::string& cfgPath) {
    ConfigEditorModule editor;
    std::string err;
    if (!editor.importConfig(cfgPath, &err))
        return emitError("error:import_config", err);

    ordered_json j;
    j["file"] = toAbsPath(cfgPath);
    j["version"] = editor.currentVersion();

    ordered_json keys = ordered_json::array();
    for (auto& k : ConfigEditorModule::editableChannelKeys()) keys.push_back(k);
    j["keys"] = keys;

    ordered_json enums = ordered_json::object();
    for (auto& [k, v] : ConfigEditorModule::enumOptions()) {
        ordered_json arr = ordered_json::array();
        for (auto& o : v) arr.push_back(o);
        enums[k] = arr;
    }
    j["enums"] = enums;

    ordered_json cards = ordered_json::object();
    for (auto& [k, v] : ConfigEditorModule::cardTypeLabels()) cards[k] = v;
    j["cards"] = cards;

    std::cout << j.dump() << "\n";
    return 0;
}

// ── get：输出配置文件 JSON 原文（不二次包装）─────────────────
static int cmdGet(const std::string& cfgPath) {
    ConfigEditorModule editor;
    std::string err;
    if (!editor.importConfig(cfgPath, &err))
        return emitError("error:import_config", err);
    std::cout << editor.getConfigJson() << "\n";
    return 0;
}

// ── save：importConfig → applyEdit(scope, card, channel, params) ─
static int cmdSave(const CmdArgs& a) {
    if (a.scope != "channel" && a.scope != "card" && a.scope != "chassis")
        return emitError("error:bad_scope",
                         "scope must be channel|card|chassis, got: " + a.scope);
    if (a.scope != "chassis" && !a.hasCard)
        return emitError("error:missing_args",
                         "--card N required for scope: " + a.scope);
    if (a.scope == "channel" && !a.hasChannel)
        return emitError("error:missing_args", "--channel M required");
    if (a.params.empty())
        return emitError("error:missing_args", "at least one --set key=value required");

    ConfigEditorModule editor;
    std::string err;
    if (!editor.importConfig(a.config, &err))
        return emitError("error:import_config", err);

    // 值原样传字符串；true/false/数字的类型转换由 applyEdit/模块内部处理
    return emitStatus(
        editor.applyEdit(a.scope, (int)a.card, (int)a.channel, a.params));
}

// ── save-app：importConfig → applyAppEdit(app_config_json) ────
static int cmdSaveApp(const CmdArgs& a) {
    if (!a.hasJson || a.json.empty())
        return emitError("error:missing_args", "--json '<app_config_json>' required");

    ConfigEditorModule editor;
    std::string err;
    if (!editor.importConfig(a.config, &err))
        return emitError("error:import_config", err);

    return emitStatus(editor.applyAppEdit(a.json));
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);

    if (argc < 2)
        return emitError("error:missing_args",
                         "usage: msm_cli_config meta|get|save|save-app [--config PATH] ...");

    const std::string cmd = argv[1];
    CmdArgs a;
    std::string err;
    if (!parseArgs(argc, argv, a, err))
        return emitError("error:bad_args", err);

    if (cmd == "meta")     return cmdMeta(a.config);
    if (cmd == "get")      return cmdGet(a.config);
    if (cmd == "save")     return cmdSave(a);
    if (cmd == "save-app") return cmdSaveApp(a);

    return emitError("error:unknown_command",
                     cmd + " (expected meta|get|save|save-app)");
}
