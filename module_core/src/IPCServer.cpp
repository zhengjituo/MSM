#include "IPCServer.h"
#include "NIStringParser.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <thread>
#include <chrono>
#include <poll.h>

namespace msm {

IPCServer::IPCServer(const std::string& socketPath)
    : Module("IPCServer"), socketPath_(socketPath) {}

IPCServer::~IPCServer() { stop(); }

// ─── Config ─────────────────────────────────────────────────────

bool IPCServer::loadConfig(const std::string& jsonPath)
{
    std::ifstream f(jsonPath);
    if (!f.is_open()) {
        std::cerr << "[IPCServer] Cannot open " << jsonPath << "\n";
        return false;
    }

    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        std::cerr << "[IPCServer] JSON parse error: " << e.what() << "\n";
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
        std::cout << "[IPCServer] Loaded config: " << cfg.name
                  << " (" << cfg.vars.size() << " vars)\n";
    }

    return true;
}

void IPCServer::setModuleMemory(
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
            vr.modPtr = static_cast<char*>(base) + vc.offset;
            varMap_[moduleName + ":" + vc.name] = vr;
        }
        break;
    }
}

// ─── Lifecycle ──────────────────────────────────────────────────

void IPCServer::start()
{
    if (running_.load()) return;

    for (auto& cfg : config_) {
        Module* mod = MessageBus::instance().findModule(cfg.name);
        if (mod && mod->getVarBase()) {
            setModuleMemory(cfg.name, mod->getVarBase(), mod->getVarSize());
        }
    }

    unlink(socketPath_.c_str());
    serverFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (serverFd_ < 0) {
        std::cerr << "[IPCServer] socket() failed\n";
        return;
    }

    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(serverFd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(serverFd_); serverFd_ = -1;
        std::cerr << "[IPCServer] bind() failed\n";
        return;
    }
    chmod(socketPath_.c_str(), 0666);

    if (listen(serverFd_, 5) < 0) {
        close(serverFd_); serverFd_ = -1;
        std::cerr << "[IPCServer] listen() failed\n";
        return;
    }

    running_ = true;
    serverThread_ = std::thread(&IPCServer::runServer, this);
    std::cout << "[IPCServer] " << socketPath_ << "\n";

    Module::start();
}

void IPCServer::stop()
{
    if (!running_.exchange(false)) return;
    if (serverFd_ >= 0) { close(serverFd_); serverFd_ = -1; }
    unlink(socketPath_.c_str());
    if (serverThread_.joinable()) serverThread_.join();
    std::cout << "[IPCServer] Stopped\n";
    Module::stop();
}

void IPCServer::handleState(const Message& msg)
{
    (void)msg;
}

ModuleVars* IPCServer::getVars()
{
    return &vars_;
}

// ─── Socket server ──────────────────────────────────────────────

void IPCServer::runServer()
{
    while (running_.load()) {
        // 先取局部副本：stop() 可能并发把 serverFd_ 关闭并置 -1，
        // 对 -1 调 FD_SET 会被 glibc fortify 判为 buffer overflow 而 abort
        int fd = serverFd_;
        if (fd < 0) break;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        timeval tv{1, 0};

        if (select(fd + 1, &fds, nullptr, nullptr, &tv) > 0
            && FD_ISSET(fd, &fds)) {
            int clientFd = accept(fd, nullptr, nullptr);
            if (clientFd >= 0) {
                handleClient(clientFd);
            }
        }
    }
}

bool IPCServer::handleClient(int clientFd)
{
    char buf[4096];
    std::string cmdBuf;
    struct pollfd pfd;
    pfd.fd = clientFd;
    pfd.events = POLLIN;

    while (running_.load()) {
        int rc = poll(&pfd, 1, 500);
        if (rc <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        memset(buf, 0, sizeof(buf));
        int n = recv(clientFd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        cmdBuf += buf;

        while (cmdBuf.find('\n') != std::string::npos) {
            size_t pos = cmdBuf.find('\n');
            std::string line = cmdBuf.substr(0, pos);
            cmdBuf.erase(0, pos + 1);
            if (line.empty()) continue;

            std::string resp = process(line) + "\n";
            send(clientFd, resp.c_str(), resp.size(), 0);
        }
    }
    close(clientFd);
    return true;
}

// ─── Command processor ──────────────────────────────────────────

std::string IPCServer::process(const std::string& line)
{
    // MSG — relay NI string to MessageBus
    if (line.size() > 4 &&
        (line[0] == 'M' || line[0] == 'm') &&
        (line[1] == 'S' || line[1] == 's') &&
        (line[2] == 'G' || line[2] == 'g') &&
        line[3] == ' ') {
        std::string niStr = line.substr(4);
        auto trim = [](std::string& s) {
            size_t st = s.find_first_not_of(" \t");
            size_t en = s.find_last_not_of(" \t");
            if (st == std::string::npos) { s.clear(); return; }
            s = s.substr(st, en - st + 1);
        };
        trim(niStr);
        if (!niStr.empty()) {
            msm::Message msg = NIStringParser::parse(niStr, "IPCServer");
            if (msg.isAwaitResponse()) {
                Message resp = MessageBus::instance().sendAndWait(msg, 1000);
                return resp.data.dump();
            }
            MessageBus::instance().send(msg);
            return "{\"OK\":\"sent\"}";
        }
        return R"({"error":"empty"})";
    }

    // QUIT — graceful shutdown
    if (line.size() > 2) {
        std::string upperLine = line;
        std::transform(upperLine.begin(), upperLine.end(), upperLine.begin(), ::toupper);
        if (upperLine == "QUIT" || upperLine == "EXIT") {
            // Post a Quit message to MessageBus — main loop checks for it
            Message quitMsg("Quit");
            quitMsg.setTarget("Controller");
            MessageBus::instance().send(quitMsg);
            return "{\"OK\":\"quitting\"}";
        }
        if (upperLine == "START") {
            Message s("Start"); s.setTarget("Controller"); MessageBus::instance().send(s);
            return "{\"OK\":\"start sent\"}";
        }
        if (upperLine == "STOP") {
            Message d("Stop"); d.setTarget("DAQModule"); MessageBus::instance().send(d);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            Message p("Stop"); p.setTarget("Process"); MessageBus::instance().send(p);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            Message s("Stop"); s.setTarget("Storage"); MessageBus::instance().send(s);
            return "{\"OK\":\"stop sent\"}";
        }
        if (upperLine == "ENABLESAVE") {
            Message s("EnableSave"); s.setTarget("Storage"); MessageBus::instance().send(s);
            Message p("EnableSave"); p.setTarget("Process"); MessageBus::instance().send(p);
            return "{\"OK\":\"enablesave sent\"}";
        }
        if (upperLine == "DISABLESAVE") {
            Message s("DisableSave"); s.setTarget("Storage"); MessageBus::instance().send(s);
            Message p("DisableSave"); p.setTarget("Process"); MessageBus::instance().send(p);
            return "{\"OK\":\"disablesave sent\"}";
        }
        if (upperLine == "STATUS") {
            Message s("Status"); s.setTarget("DAQModule"); s.setAwaitResponse(true);
            Message resp = MessageBus::instance().sendAndWait(s, 1000);
            return resp.data.dump();
        }
    }

    std::istringstream iss(line);
    std::string cmd, target, value;
    iss >> cmd >> target >> value;

    auto upper = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::toupper);
        return s;
    };
    cmd = upper(cmd);

    // LIST — list all modules and variables from config
    if (cmd == "LIST" || cmd == "LS") {
        std::string result = "{\"OK\":{";
        for (size_t i = 0; i < config_.size(); i++) {
            if (i > 0) result += ",";
            result += "\"" + config_[i].name + "\":{";
            for (size_t j = 0; j < config_[i].vars.size(); j++) {
                if (j > 0) result += ",";
                auto& v = config_[i].vars[j];
                std::string key = config_[i].name + ":" + v.name;
                auto it = varMap_.find(key);
                std::string val = (it != varMap_.end())
                    ? readMemory(it->second.modPtr, it->second.type)
                    : "?";
                result += "\"" + v.name + "\":\"" + v.type + ":" + val + "\"";
            }
            result += "}";
        }
        result += "}}";
        return result;
    }

    // SCHEMA — show module/variable schema
    if (cmd == "SCHEMA") {
        auto colon = target.find(':');
        std::string mname = (colon != std::string::npos) ? target.substr(0, colon) : target;
        std::string vname = (colon != std::string::npos) ? target.substr(colon + 1) : "*";

        for (auto& mc : config_) {
            if (mc.name != mname) continue;

            if (vname == "*" || vname.empty()) {
                std::string result = "{\"OK\":{\"module\":\"" + mname
                    + "\",\"memory_size\":" + std::to_string(mc.memorySize)
                    + ",\"variables\":[";
                for (size_t j = 0; j < mc.vars.size(); j++) {
                    if (j > 0) result += ",";
                    auto& v = mc.vars[j];
                    result += "{\"name\":\"" + v.name + "\",\"type\":\"" + v.type
                        + "\",\"offset\":" + std::to_string(v.offset)
                        + ",\"len\":" + std::to_string(v.len)
                        + ",\"dir\":\"" + v.dir + "\"}";
                }
                result += "]}}";
                return result;
            }

            for (auto& v : mc.vars) {
                if (v.name != vname) continue;
                return "{\"OK\":{\"name\":\"" + v.name + "\",\"type\":\"" + v.type
                    + "\",\"offset\":" + std::to_string(v.offset)
                    + ",\"len\":" + std::to_string(v.len)
                    + ",\"dir\":\"" + v.dir + "\"}}";
            }
            return R"({"error":"Variable not found"})";
        }
        return R"({"error":"Module not found"})";
    }

    // All remaining commands need "Module:var" format
    auto colon = target.find(':');
    if (colon == std::string::npos) return R"({"error":"Use M:V format"})";

    std::string key = target;  // "ModuleName:varName"

    if (cmd == "GET") {
        auto it = varMap_.find(key);
        if (it == varMap_.end()) return R"({"error":"Variable not found"})";
        return "{\"OK\":" + readMemory(it->second.modPtr, it->second.type) + "}";
    }

    if (cmd == "PUT") {
        auto it = varMap_.find(key);
        if (it == varMap_.end()) return R"({"error":"Variable not found"})";

        auto& vr = it->second;
        if (vr.dir != "i" && vr.dir != "io")
            return R"({"error":"Read-only variable"})";

        writeMemory(vr.modPtr, vr.type, value);

        std::string vname = target.substr(colon + 1);
        if (vname == "action") {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::string newVal = readMemory(vr.modPtr, vr.type);
        return "{\"OK\":" + newVal + "}";
    }

    return R"({"error":"Use GET/PUT/LIST/SCHEMA"})";
}

// ─── Memory helpers ─────────────────────────────────────────────

void IPCServer::writeMemory(void* ptr, const std::string& type, const std::string& value)
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

std::string IPCServer::readMemory(void* ptr, const std::string& type)
{
    if (!ptr) return "";
    if (type == "int") return std::to_string(*static_cast<int*>(ptr));
    if (type == "double") {
        std::ostringstream ss; ss << *static_cast<double*>(ptr); return ss.str();
    }
    if (type == "string") return *static_cast<std::string*>(ptr);
    return "";
}

} // namespace msm
