#include "httplib.h"
#include "config_editor_module.h"
#include "MessageBus.h"
#include "Message.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <csignal>
#include <unistd.h>

using msm::ConfigEditorModule;
using msm::MessageBus;
using msm::Message;

static std::atomic<bool> g_stop{false};

// 解析 exe 路径，向上找 config/app_runtime_config.json（主操作文件）
static std::string resolveConfigPath(const char* filename = "app_runtime_config.json") {
    char exe[1024];
    ssize_t len = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    std::string base;
    if (len > 0) {
        exe[len] = 0;
        std::string ep(exe);
        size_t s = ep.rfind('/');
        if (s != std::string::npos) base = ep.substr(0, s);
    }
    // main_config_editor/build/msm_config_editor → ../../config/app_runtime_config.json
    std::string candidate = base + "/../../config/" + filename;
    // 简化路径归一化（处理 /../）
    size_t pos;
    while ((pos = candidate.find("/../")) != std::string::npos) {
        if (pos == 0) { candidate.erase(0, 3); continue; }
        size_t prev = candidate.rfind('/', pos - 1);
        if (prev == std::string::npos) prev = 0;
        candidate.erase(prev, pos + 3 - prev);
    }
    if (::access(candidate.c_str(), R_OK) == 0) return candidate;
    std::string fallback = std::string("config/") + filename;
    return fallback;
}

// 解析配置文件所在目录（用于定位同目录的 initial 配置）
static std::string dirName(const std::string& path) {
    size_t s = path.rfind('/');
    return s == std::string::npos ? "." : path.substr(0, s);
}

// 基于 exe 路径定位 web 页面（main_config_editor/build/msm_config_editor → ../web/config_editor.html）
static std::string resolveWebPath() {
    char exe[1024];
    ssize_t len = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    std::string base;
    if (len > 0) {
        exe[len] = 0;
        std::string ep(exe);
        size_t s = ep.rfind('/');
        if (s != std::string::npos) base = ep.substr(0, s);
    }
    std::string candidate = base + "/../web/config_editor.html";
    size_t pos;
    while ((pos = candidate.find("/../")) != std::string::npos) {
        if (pos == 0) { candidate.erase(0, 3); continue; }
        size_t prev = candidate.rfind('/', pos - 1);
        if (prev == std::string::npos) prev = 0;
        candidate.erase(prev, pos + 3 - prev);
    }
    if (::access(candidate.c_str(), R_OK) == 0) return candidate;
    std::string fallback = "config_editor.html";
    return fallback;
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, [](int) { g_stop.store(true); });
    signal(SIGTERM, [](int) { g_stop.store(true); });
    signal(SIGHUP, [](int) { g_stop.store(true); });

    // 可选参数：配置文件路径（默认 app_runtime_config.json）
    std::string cfgArg;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--config" && i + 1 < argc) cfgArg = argv[i + 1];
    }

    auto& bus = MessageBus::instance();
    ConfigEditorModule editor;
    bus.registerModule(&editor);
    editor.start();

    // 主操作文件 = app_runtime_config.json；启动配置（可勾选写入）= app_initial_config.json
    std::string cfgPath = cfgArg.empty() ? resolveConfigPath() : cfgArg;
    std::string initPath = dirName(cfgPath) + "/app_initial_config.json";
    std::string err;
    if (!editor.importConfig(cfgPath, &err)) {
        std::cerr << "[config-editor] import failed: " << err << "\n";
        // 继续启动，页面可显示错误
    } else {
        std::cout << "[config-editor] loaded: " << cfgPath
                  << " (version " << editor.currentVersion() << ")\n";
    }

    httplib::Server svr;

    // ── 页面 ──
    std::string webPath = resolveWebPath();
    svr.Get("/", [webPath](const httplib::Request&, httplib::Response& res) {
        std::ifstream f(webPath);
        if (!f.is_open()) { res.status = 404; res.set_content("config_editor.html not found", "text/plain"); return; }
        std::ostringstream oss; oss << f.rdbuf();
        res.set_content(oss.str(), "text/html");
    });

    // ── 查询 ──
    svr.Get("/api/config", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(editor.getConfigJson(), "application/json");
    });
    svr.Get("/api/lock", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(editor.getAcquiringJson(), "application/json");
    });
    svr.Get("/api/meta", [&](const httplib::Request&, httplib::Response& res) {
        // 前端下拉所需元数据
        std::string meta = R"({"keys":[)";
        auto& keys = ConfigEditorModule::editableChannelKeys();
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i) meta += ",";
            meta += "\"" + keys[i] + "\"";
        }
        meta += "],\"enums\":{";
        bool first = true;
        for (auto& [k, opts] : ConfigEditorModule::enumOptions()) {
            if (!first) meta += ",";
            first = false;
            meta += "\"" + k + "\":[";
            for (size_t i = 0; i < opts.size(); ++i) {
                if (i) meta += ",";
                meta += "\"" + opts[i] + "\"";
            }
            meta += "]";
        }
        meta += "},\"cards\":{";
        first = true;
        for (auto& [k, v] : ConfigEditorModule::cardTypeLabels()) {
            if (!first) meta += ",";
            first = false;
            meta += "\"" + k + "\":\"" + v + "\"";
        }
        meta += "},\"version\":\"" + editor.currentVersion() + "\"";
        meta += ",\"runtimeFile\":\"" + cfgPath + "\"";
        meta += ",\"initFile\":\"" + initPath + "\"}";
        res.set_content(meta, "application/json");
    });

    // ── 保存（channel/card/chassis 三种 scope）──
    svr.Post("/api/config/save", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = nlohmann::ordered_json::parse(req.body);
            std::string scope = j.value("scope", "channel");
            int cardIndex = j.value("cardIndex", 0);
            int channel = j.value("channel", 0);
            std::map<std::string, std::string> params;
            if (j.contains("params")) {
                for (auto& [k, v] : j["params"].items()) {
                    if (v.is_boolean()) params[k] = v.get<bool>() ? "true" : "false";
                    else params[k] = v.is_string() ? v.get<std::string>() : v.dump();
                }
            }
            std::string status = editor.applyEdit(scope, cardIndex, channel, params);
            if (status == "ok") { res.set_content(R"({"status":"ok"})", "application/json"); return; }
            if (status == "locked") { res.status = 409; res.set_content(R"({"status":"locked"})", "application/json"); return; }
            res.status = 400;
            res.set_content("{\"status\":\"" + status + "\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(std::string("{\"status\":\"error\",\"msg\":\"") + e.what() + "\"}", "application/json");
        }
    });

    // ── 保存 app_config ──
    svr.Post("/api/config/save-app", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = nlohmann::ordered_json::parse(req.body);
            std::string appJson;
            if (j.contains("app_config")) appJson = j["app_config"].dump();
            else appJson = req.body;
            // 主操作文件（runtime）
            std::string status = editor.applyAppEdit(appJson);
            if (status == "locked") { res.status = 409; res.set_content(R"({"status":"locked"})", "application/json"); return; }
            if (status.rfind("error", 0) == 0) { res.status = 400; res.set_content("{\"status\":\"" + status + "\"}", "application/json"); return; }
            // 勾选「保存到启动配置」→ 额外写 app_initial_config.json
            bool saveToInit = j.value("saveToInit", false);
            if (saveToInit) {
                std::string st2 = editor.applyAppEditToFile(appJson, initPath);
                if (st2 == "locked") { res.status = 409; res.set_content(R"({"status":"locked"})", "application/json"); return; }
                if (st2.rfind("error", 0) == 0) { res.status = 400; res.set_content("{\"status\":\"" + st2 + "\"}", "application/json"); return; }
            }
            res.set_content(R"({"status":"ok"})", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(std::string("{\"status\":\"error\",\"msg\":\"") + e.what() + "\"}", "application/json");
        }
    });

    // ── 退出 ──
    svr.Post("/api/shutdown", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"bye"})", "application/json");
        g_stop.store(true);
    });

    std::thread httpThread([&] { svr.listen("0.0.0.0", 8081); });
    std::cout << "[config-editor] listening on http://localhost:8081\n";

    while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    svr.stop();
    if (httpThread.joinable()) httpThread.join();
    editor.stop();
    std::cout << "[config-editor] shutdown\n";
    return 0;
}
