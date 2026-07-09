#include "RESTServer.h"
#include "NIStringParser.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <set>

namespace msm {

RESTServer::RESTServer(int port)
    : Module("RESTServer"), port_(port) {}

RESTServer::~RESTServer()
{
    stop();
}

// ─── Config ─────────────────────────────────────────────────────

bool RESTServer::loadConfig(const std::string& jsonPath)
{
    std::ifstream f(jsonPath);
    if (!f.is_open()) {
        std::cerr << "[RESTServer] Cannot open " << jsonPath << "\n";
        return false;
    }

    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        std::cerr << "[RESTServer] JSON parse error: " << e.what() << "\n";
        return false;
    }

    for (auto& jmod : j["modules"]) {
        ModuleConfig cfg;
        cfg.name = jmod["name"];
        cfg.memorySize = jmod.value("memory_size", 0);

        for (auto& jvar : jmod["variables"]) {
            VarConfig vc;
            vc.name = jvar["name"];
            vc.type = jvar["type"];
            vc.offset = jvar["offset"];
            vc.len = jvar.value("len", 4);
            vc.dir = jvar.value("dir", "io");
            cfg.vars.push_back(vc);
        }
        config_.push_back(cfg);
        std::cout << "[RESTServer] Loaded config: " << cfg.name
                  << " (" << cfg.vars.size() << " vars)\n";
    }

    return true;
}

void RESTServer::setModuleMemory(
    const std::string& moduleName, void* base, size_t size)
{
    (void)size;
    for (auto& cfg : config_) {
        if (cfg.name != moduleName) continue;
        for (auto& vc : cfg.vars) {
            VarRuntime vr;
            vr.type = vc.type;
            vr.dir = vc.dir;
            vr.offset = vc.offset;
            vr.length = vc.len;
            bool isArray = vc.type.size() > 4 && vc.type.compare(vc.type.size()-2, 2, "[]") == 0;
            vr.nelm = isArray ? (vc.len / (vc.type[0] == 'd' ? 8 : 4)) : 0;
            vr.modPtr = static_cast<char*>(base) + vc.offset;
            varMap_[moduleName + ":" + vc.name] = vr;
        }
        break;
    }
}

// ─── Lifecycle ──────────────────────────────────────────────────

void RESTServer::start()
{
    if (httpRunning_.load()) return;

    for (auto& cfg : config_) {
        Module* mod = MessageBus::instance().findModule(cfg.name);
        if (mod && mod->getVarBase()) {
            setModuleMemory(cfg.name, mod->getVarBase(), mod->getVarSize());
        }
    }

    // GET /api/:module/:var
    svr_.Get(R"(/api/([^/]+)/([^/]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
        std::string module = req.matches[1];
        std::string var = req.matches[2];
        std::string key = module + ":" + var;

        auto it = varMap_.find(key);
        if (it == varMap_.end()) {
            res.set_content(jsonError("Variable not found"), "application/json");
            return;
        }
        res.set_content(
            jsonResponse(true, readMemory(it->second.modPtr, it->second.type)),
            "application/json");
    });

    // PUT /api/:module/:var
    svr_.Put(R"(/api/([^/]+)/([^/]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
        std::string module = req.matches[1];
        std::string var = req.matches[2];
        std::string key = module + ":" + var;

        auto it = varMap_.find(key);
        if (it == varMap_.end()) {
            res.set_content(jsonError("Variable not found"), "application/json");
            return;
        }

        auto& vr = it->second;
        if (vr.dir != "i" && vr.dir != "io") {
            res.set_content(jsonError("Read-only variable"), "application/json");
            return;
        }

        writeMemory(vr.modPtr, vr.type, req.body);

        res.set_content(
            jsonResponse(true, readMemory(vr.modPtr, vr.type)),
            "application/json");
    });

    // POST /api/:module/action — write variables and read outputs
    svr_.Post(R"(/api/([^/]+)/action)",
        [this](const httplib::Request& req, httplib::Response& res) {
        std::string module = req.matches[1];

        nlohmann::json j;
        try {
            j = nlohmann::json::parse(req.body);
        } catch (...) {
            res.set_content(jsonError("Invalid JSON body"), "application/json");
            return;
        }

        std::string prefix = module + ":";
        for (auto& [key, val] : j.items()) {
            std::string pvName = prefix + key;
            auto it = varMap_.find(pvName);
            if (it != varMap_.end() && (it->second.dir == "i" || it->second.dir == "io")) {
                std::string sv;
                if (val.is_string()) sv = val.get<std::string>();
                else if (val.is_number()) sv = std::to_string(val.get<int>());
                if (!sv.empty()) writeMemory(it->second.modPtr, it->second.type, sv);
            }
        }

        nlohmann::json result;
        result["status"] = "ok";
        result["module"] = module;

        for (auto& [key, vr] : varMap_) {
            if (key.find(prefix) == 0 && (vr.dir == "o" || vr.dir == "io")) {
                std::string vname = key.substr(prefix.size());
                result["variables"][vname] = readMemory(vr.modPtr, vr.type);
            }
        }

        res.set_content(result.dump(), "application/json");
    });

    // POST /api/msg — relay NI string message
    svr_.Post("/api/msg",
        [](const httplib::Request& req, httplib::Response& res) {
        const std::string& body = req.body;
        if (body.empty()) {
            res.set_content("{\"error\":\"empty body\"}", "application/json");
            return;
        }
        msm::Message msg = msm::NIStringParser::parse(body, "RESTServer");
        msm::MessageBus::instance().send(msg);
        nlohmann::json j;
        j["status"] = "sent";
        j["cmd"] = msg.cmd;
        j["target"] = msg.getTarget();
        res.set_content(j.dump(), "application/json");
    });

    // GET /api/schema — only return modules present in varMap_
    svr_.Get("/api/schema",
        [this](const httplib::Request&, httplib::Response& res) {
        std::set<std::string> activeMods;
        for (auto& [key, _] : varMap_) {
            auto colon = key.find(':');
            if (colon != std::string::npos)
                activeMods.insert(key.substr(0, colon));
        }
        nlohmann::json j;
        for (auto& mc : config_) {
            if (!activeMods.count(mc.name)) continue;
            nlohmann::json modInfo;
            modInfo["memory_size"] = mc.memorySize;
            modInfo["variables"] = nlohmann::json::array();
            for (auto& vc : mc.vars) {
                nlohmann::json vInfo;
                vInfo["name"] = vc.name;
                vInfo["type"] = vc.type;
                vInfo["offset"] = vc.offset;
                vInfo["len"] = vc.len;
                vInfo["dir"] = vc.dir;
                modInfo["variables"].push_back(vInfo);
            }
            j[mc.name] = modInfo;
        }
        res.set_content(j.dump(2), "application/json");
    });

    // GET /api/:module/schema
    svr_.Get(R"(/api/([^/]+)/schema)",
        [this](const httplib::Request& req, httplib::Response& res) {
        std::string module = req.matches[1];
        for (auto& mc : config_) {
            if (mc.name != module) continue;
            nlohmann::json j;
            j["name"] = mc.name;
            j["memory_size"] = mc.memorySize;
            j["variables"] = nlohmann::json::array();
            for (auto& vc : mc.vars) {
                nlohmann::json vInfo;
                vInfo["name"] = vc.name;
                vInfo["type"] = vc.type;
                vInfo["offset"] = vc.offset;
                vInfo["len"] = vc.len;
                vInfo["dir"] = vc.dir;
                j["variables"].push_back(vInfo);
            }
            res.set_content(j.dump(2), "application/json");
            return;
        }
        res.set_content(jsonError("Module not found"), "application/json");
    });

    // GET /api/:module — list all variables with values
    svr_.Get(R"(/api/([^/]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
        std::string module = req.matches[1];
        std::string prefix = module + ":";

        nlohmann::json j;
        j["module"] = module;
        j["variables"] = nlohmann::json::object();

        bool found = false;
        for (auto& [key, vr] : varMap_) {
            if (key.find(prefix) == 0) {
                found = true;
                std::string vname = key.substr(prefix.size());
                j["variables"][vname] = readMemory(vr.modPtr, vr.type);
            }
        }

        if (!found) {
            res.set_content(jsonError("Module not found"), "application/json");
            return;
        }
        res.set_content(j.dump(2), "application/json");
    });

    if (!staticDir_.empty()) {
        svr_.Get("/", [this](const httplib::Request&, httplib::Response& res) {
            std::ifstream f(staticDir_ + "/msm_dashboard.html");
            if (f) {
                std::string content((std::istreambuf_iterator<char>(f)), {});
                res.set_content(content, "text/html");
            } else {
                res.status = 404;
            }
        });
        svr_.Get("/msm_dashboard.html", [this](const httplib::Request&, httplib::Response& res) {
            std::ifstream f(staticDir_ + "/msm_dashboard.html");
            if (f) {
                std::string content((std::istreambuf_iterator<char>(f)), {});
                res.set_content(content, "text/html");
            } else {
                res.status = 404;
            }
        });
        std::cout << "[RESTServer] GUI at http://localhost:" << port_ << "/\n";
    }

    httpRunning_ = true;
    httpThread_ = std::thread([this]() {
        std::cout << "[RESTServer] Listening on port " << port_ << "\n";
        svr_.listen("0.0.0.0", port_);
    });

    Module::start();
}

void RESTServer::enableStaticDir(const std::string& dir) {
    staticDir_ = dir;
}

void RESTServer::stop()
{
    if (!httpRunning_.exchange(false)) return;
    svr_.stop();
    if (httpThread_.joinable()) httpThread_.join();
    std::cout << "[RESTServer] Stopped\n";
    Module::stop();
}

void RESTServer::handleState(const Message& msg)
{
    (void)msg;
}

ModuleVars* RESTServer::getVars()
{
    return &vars_;
}

// ─── Memory helpers ─────────────────────────────────────────────

void RESTServer::writeMemory(void* ptr, const std::string& type, const std::string& value)
{
    if (!ptr) return;
    try {
        if (type == "int") {
            *static_cast<int*>(ptr) = std::stoi(value);
        } else if (type == "double") {
            *static_cast<double*>(ptr) = std::stod(value);
        } else if (type == "string") {
            *static_cast<std::string*>(ptr) = value;
        }
    } catch (...) {}
}

std::string RESTServer::readMemory(void* ptr, const std::string& type)
{
    if (!ptr) return "";
    if (type == "int") return std::to_string(*static_cast<int*>(ptr));
    if (type == "double") {
        std::ostringstream ss; ss << *static_cast<double*>(ptr); return ss.str();
    }
    if (type == "string") return *static_cast<std::string*>(ptr);
    if (type == "double[]") {
        auto* arr = static_cast<double*>(ptr);
        std::ostringstream ss; ss << "[";
        for (size_t i = 0; i < 10; i++) {
            if (i > 0) ss << ",";
            ss << arr[i];
        }
        ss << "]";
        return ss.str();
    }
    if (type == "int[]") {
        auto* arr = static_cast<int*>(ptr);
        std::ostringstream ss; ss << "[";
        for (size_t i = 0; i < 10; i++) {
            if (i > 0) ss << ",";
            ss << arr[i];
        }
        ss << "]";
        return ss.str();
    }
    return "";
}

std::string RESTServer::jsonResponse(bool ok, const std::string& value)
{
    nlohmann::json j;
    j[ok ? "ok" : "error"] = value;
    return j.dump();
}

std::string RESTServer::jsonError(const std::string& msg)
{
    nlohmann::json j;
    j["error"] = msg;
    return j.dump();
}

} // namespace msm
