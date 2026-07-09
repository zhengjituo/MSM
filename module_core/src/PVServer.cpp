#include "PVServer.h"
#include "MessageBus.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <cstring>
#include <cadef.h>
#include <db_access.h>

namespace msm {

// ─── CA callbacks ─────────────────────────────────────────────

struct SubUser {
    PVServer* server;
    std::string pvName;
};

static void caConnectCallback(struct connection_handler_args args) {
    auto* ctx = static_cast<CAChannelContext*>(ca_puser(args.chid));
    std::cout << "[PVServer] caConnectCallback chid=" << args.chid
              << " ctx=" << ctx << " op=" << args.op << "\n";
    if (!ctx) return;
    ctx->connected = (args.op == CA_OP_CONN_UP);
    ctx->disconnected = (args.op == CA_OP_CONN_DOWN);
    if (ctx->connected) {
        std::cout << "[PVServer] CA connected: " << ctx->pvName << "\n";
    } else if (ctx->disconnected) {
        std::cout << "[PVServer] CA disconnected: " << ctx->pvName << "\n";
    }
}

static void caMonitorCallback(struct event_handler_args args) {
    if (args.status != ECA_NORMAL || !args.dbr) return;
    auto* user = static_cast<SubUser*>(args.usr);
    if (user && user->server) {
        user->server->onMonitorUpdate(user->pvName, args.type, args.dbr);
    }
}

// ─── PVServer ─────────────────────────────────────────────────

PVServer::PVServer(const std::string& pvPrefix)
    : Module("PVServer"), prefix_(pvPrefix) {}

PVServer::~PVServer() { stop(); }

bool PVServer::loadConfig(const std::string& jsonPath)
{
    std::ifstream f(jsonPath);
    if (!f.is_open()) {
        std::cerr << "[PVServer] Cannot open " << jsonPath << "\n";
        return false;
    }

    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        std::cerr << "[PVServer] JSON parse error: " << e.what() << "\n";
        return false;
    }

    prefix_ = j.value("prefix", prefix_);
    snapshotInterval_ = j.value("snapshot_interval", 5);

    for (auto& jmod : j["modules"]) {
        PVModuleConfig cfg;
        cfg.name = jmod["name"];
        cfg.memorySize = jmod.value("memory_size", 0);

        for (auto& jvar : jmod["variables"]) {
            PVVarConfig vc;
            vc.name = jvar["name"];
            vc.type = jvar["type"];
            vc.offset = jvar["offset"];
            vc.len = jvar.value("len", 4);
            vc.nelm = jvar.value("nelm", 0);
            vc.dir = jvar.value("dir", "io");
            vc.epics = true;
            if (jvar.contains("epics")) {
                auto& ef = jvar["epics"];
                if (ef.is_boolean()) vc.epics = ef.get<bool>();
                else vc.epics = ef.get<int>() != 0;
            }
            vc.pvName = jvar.value("pv", "");
            if (vc.pvName.empty()) {
                vc.pvName = prefix_ + ":" + cfg.name + ":" + vc.name;
            }
            cfg.vars.push_back(vc);
        }
        config_.push_back(cfg);
        std::cout << "[PVServer] Loaded config: " << cfg.name
                  << " (" << cfg.vars.size() << " vars)\n";
    }

    return true;
}

void PVServer::setModuleMemory(
    const std::string& moduleName, void* base, size_t size)
{
    PVModuleRuntime rt;
    rt.name = moduleName;
    rt.basePtr = base;
    rt.memorySize = size;
    modules_.push_back(rt);

    for (auto& cfg : config_) {
        if (cfg.name != moduleName) continue;
        for (auto& vc : cfg.vars) {
            if (!vc.epics) continue;

            PVVarRuntime vr;
            vr.moduleName = moduleName;
            vr.varName = vc.name;
            vr.type = vc.type;
            vr.dir = vc.dir;
            vr.offset = vc.offset;
            vr.length = vc.len;
            vr.modPtr = static_cast<char*>(base) + vc.offset;
            vr.isArray = vc.nelm > 0;
            vr.nelm = vc.nelm > 0 ? vc.nelm : 1;

            varMap_[vc.pvName] = vr;
            std::cout << "[PVServer] PV " << vc.pvName
                      << " (" << vc.type << ", dir:" << vc.dir << ")\n";
        }
        break;
    }
}

// ─── Start / Stop ──────────────────────────────────────────────

void PVServer::start()
{
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    for (auto& cfg : config_) {
        Module* mod = MessageBus::instance().findModule(cfg.name);
        if (mod && mod->getVarBase()) {
            setModuleMemory(cfg.name, mod->getVarBase(), mod->getVarSize());
        }
    }

    Module::start();
    // NOTE: restore() intentionally omitted here — it would overwrite module memory
    // that was already initialized by Configure messages. Modules push their own
    // state to PVs via PV_UPDATE after PVServer is fully started (see main).

    if (varMap_.empty()) {
        std::cout << "[PVServer] No PVs to serve\n";
        return;
    }

    if (iocEnabled_) {
        generateDB(iocDBPath_);
        if (spawnIOC() < 0) {
            std::cerr << "[PVServer] Failed to start IOC\n";
            return;
        }
        // Wait for IOC to be ready and CA server listening
        std::cout << "[PVServer] Waiting for IOC to start...\n";
    for (int i = 0; i < 5; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            // Try connecting to IOC's CA port
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock >= 0) {
                struct sockaddr_in addr;
                memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_port = htons(5064);
                addr.sin_addr.s_addr = inet_addr("127.0.0.1");
                if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                    close(sock);
                    break;
                }
                close(sock);
            }
        }
        if (!caInitChannels()) {
            std::cerr << "[PVServer] CA channel init failed\n";
        }
    }

    if (snapshotInterval_ > 0) {
        snapshotRunning_ = true;
        snapshotThread_ = std::thread([this]() {
            while (snapshotRunning_) {
                std::this_thread::sleep_for(
                    std::chrono::seconds(snapshotInterval_));
                if (!snapshot("msm_vars.snap")) {
                    std::cerr << "[PVServer] WARNING: snapshot failed\n";
                }
            }
        });
    }
}

void PVServer::stop()
{
    if (stopped_.exchange(true)) return;
    snapshotRunning_ = false;
    if (snapshotThread_.joinable()) snapshotThread_.join();

    caCleanup();
    killIOC();

    snapshot("msm_vars.snap");

    varMap_.clear();
    modules_.clear();
    config_.clear();

    Module::stop();
}

// ─── CA init / cleanup ────────────────────────────────────────

bool PVServer::caInitChannels()
{
    setenv("EPICS_CA_ADDR_LIST", "127.0.0.1:5064", 0);
    setenv("EPICS_CA_AUTO_ADDR_LIST", "NO", 0);

    if (ca_context_create(ca_enable_preemptive_callback) != ECA_NORMAL) {
        std::cerr << "[PVServer] ca_context_create failed\n";
        return false;
    }

    std::cout << "[PVServer] Creating CA channels...\n";
    int n = 0;
    for (auto& [pvName, vr] : varMap_) {
        CAChannelContext& ctx = caChannels_[pvName];
        ctx.pvName = pvName;
        ctx.channel = nullptr;

        chid chan;
        int ret = ca_create_channel(pvName.c_str(), caConnectCallback,
                                     &ctx, 20, &chan);
        if (ret != ECA_NORMAL) {
            std::cerr << "[PVServer] ca_create_channel failed: " << pvName
                      << " (ret=" << ret << ")\n";
            caChannels_.erase(pvName);
            continue;
        }
        ctx.channel = chan;
        n++;
    }
    ca_flush_io();

    // Process events until all channels connect (max 10s)
    int connected = 0;
    for (int wait = 0; wait < 100 && connected < (int)caChannels_.size(); wait++) {
        ca_pend_event(0.1);
        connected = 0;
        for (auto& [pv, ctx] : caChannels_) {
            if (ctx.channel) {
                int s = ca_state(static_cast<chid>(ctx.channel));
                if (s == cs_conn) connected++;
            }
        }
    }
    std::cout << "[PVServer] CA connected " << connected << "/" << caChannels_.size()
              << " (after " << (connected==(int)caChannels_.size() ? "all" : "some") << ")\n";
    for (auto& [pv, ctx] : caChannels_) {
        int s = ctx.channel ? ca_state(static_cast<chid>(ctx.channel)) : -1;
        std::cout << "[PVServer]   " << pv << " state=" << s << "\n";
    }
    std::cout << "[PVServer] CA connected " << connected << "/" << n << " channels\n";

    subscribeAll();

    pushAllToIOC();

    return connected > 0;
}

void PVServer::subscribeAll()
{
    for (auto& [pvName, vr] : varMap_) {
        auto it = caChannels_.find(pvName);
        if (it == caChannels_.end()) continue;
        auto& ctx = it->second;
        if (!ctx.channel || !ctx.connected) continue;

        if (vr.dir != "i" && vr.dir != "io") continue;
        if (ctx.subscribed) continue;

        long dbrType = DBR_LONG;
        if (vr.type == "double" || vr.type == "double[]") dbrType = DBR_DOUBLE;

        auto* user = new SubUser{this, pvName};
        ctx.subUser = user;

        evid ev;
        int ret = ca_create_subscription(dbrType, vr.nelm,
            static_cast<chid>(ctx.channel), DBE_VALUE,
            caMonitorCallback, user, &ev);
        if (ret == ECA_NORMAL) {
            ctx.subscription = ev;
            ctx.subscribed = true;
            std::cout << "[PVServer] Subscribed: " << pvName << "\n";
        } else {
            std::cerr << "[PVServer] Subscription failed: " << pvName << "\n";
            delete user;
            ctx.subUser = nullptr;
        }
    }
    ca_flush_io();
}

void PVServer::caCleanup()
{
    for (auto& [pvName, ctx] : caChannels_) {
        if (ctx.subscription) {
            ca_clear_subscription(static_cast<evid>(ctx.subscription));
        }
        if (ctx.subUser) {
            delete static_cast<SubUser*>(ctx.subUser);
        }
        if (ctx.channel) {
            ca_clear_channel(static_cast<chid>(ctx.channel));
        }
    }
    caChannels_.clear();
    ca_context_destroy();
}

// ─── IOC subprocess ────────────────────────────────────────────

pid_t PVServer::spawnIOC()
{
    std::string iocPath = iocEpicsBase_ + "/bin/linux-x86_64/softIocPVA";

    int stdinPipe[2];
    if (pipe(stdinPipe) != 0) {
        std::cerr << "[PVServer] pipe() failed\n";
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "[PVServer] fork() failed\n";
        close(stdinPipe[0]);
        close(stdinPipe[1]);
        return -1;
    }

    if (pid == 0) {
        close(stdinPipe[1]);
        dup2(stdinPipe[0], STDIN_FILENO);
        close(stdinPipe[0]);

        setenv("EPICS_CAS_INTF_ADDR_LIST", "127.0.0.1", 1);
        setenv("EPICS_CA_SERVER_PORT", "5064", 1);

        execlp(iocPath.c_str(), "softIocPVA", "-d", iocDBPath_.c_str(), nullptr);
        std::cerr << "[PVServer] execlp failed: " << iocPath << "\n";
        _exit(1);
    }

    close(stdinPipe[0]);
    iocStdin_ = stdinPipe[1];
    iocPID_ = pid;
    std::cout << "[PVServer] IOC started (PID " << pid << ")\n";
    return pid;
}

void PVServer::killIOC()
{
    if (iocPID_ <= 0) return;
    if (iocStdin_ >= 0) close(iocStdin_);
    iocStdin_ = -1;
    kill(iocPID_, SIGTERM);
    int status;
    waitpid(iocPID_, &status, 0);
    std::cout << "[PVServer] IOC stopped (PID " << iocPID_ << ")\n";
    iocPID_ = -1;
}

// ─── Message handling ──────────────────────────────────────────

void PVServer::handleState(const Message& msg)
{
    if (msg.cmd == "PV_UPDATE") {
        std::string moduleName = msg.data.get("module");
        std::string varName = msg.data.get("var");

        if (varName == "*") {
            pushAllToIOC();
            return;
        }
        for (auto& [pvName, vr] : varMap_) {
            if (vr.moduleName == moduleName && vr.varName == varName) {
                if (iocEnabled_ && iocPID_ > 0) {
                    pushToIOC(pvName);
                }
            }
        }
        return;
    }


}

ModuleVars* PVServer::getVars() { return &vars_; }

void PVServer::pushToIOC(const std::string& pvName)
{
    auto it = varMap_.find(pvName);
    if (it == varMap_.end()) return;
    auto& vr = it->second;

    // Create a fresh channel for the put, then destroy it.
    // Using permanent channels (caChannels_) for put causes CA interaction
    // issues with subscriptions on the same channel.
    chid chan = nullptr;
    int ret = ca_create_channel(pvName.c_str(), nullptr, nullptr, 20, &chan);
    if (ret != ECA_NORMAL) {
        std::cerr << "[PVServer] pushToIOC: create_channel failed for " << pvName << "\n";
        return;
    }
    ca_flush_io();
    for (int i = 0; i < 20; i++) {
        ca_pend_event(0.1);
        if (ca_state(chan) == cs_conn) break;
    }
    if (ca_state(chan) != cs_conn) {
        std::cerr << "[PVServer] pushToIOC: " << pvName << " not connected\n";
        ca_clear_channel(chan);
        return;
    }

    long dbrType = DBR_LONG;
    if (vr.type == "double" || vr.type == "double[]") dbrType = DBR_DOUBLE;

    if (vr.isArray) {
        ret = ca_array_put(dbrType, vr.nelm, chan, vr.modPtr);
    } else if (dbrType == DBR_DOUBLE) {
        double val = *static_cast<double*>(vr.modPtr);
        ret = ca_array_put(DBR_DOUBLE, 1, chan, &val);
    } else {
        dbr_long_t val = static_cast<dbr_long_t>(*static_cast<int*>(vr.modPtr));
        ret = ca_array_put(DBR_LONG, 1, chan, &val);
    }
    ca_flush_io();
    ca_pend_io(1.0);

    ca_clear_channel(chan);

    if (ret != ECA_NORMAL) {
        std::cerr << "[PVServer] pushToIOC " << pvName
                  << " FAILED: " << ca_message(ret) << "\n";
    } else {
        std::cerr << "[PVServer] pushToIOC OK: " << pvName
                  << " val=" << readMemory(vr.modPtr, vr.type) << "\n";
    }
}

// Batch pushToIOC for all PVs with a small delay between each to avoid
// overwhelming the IOC with temporary channel create/destroy.
void PVServer::pushAllToIOC()
{
    for (auto& [pvName, vr] : varMap_) {
        pushToIOC(pvName);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void PVServer::onMonitorUpdate(const std::string& pvName, long dbrType, const void* data)
{
    auto it = varMap_.find(pvName);
    if (it == varMap_.end()) return;
    auto& vr = it->second;
    if (vr.dir != "i" && vr.dir != "io") return;

    auto cit = caChannels_.find(pvName);
    if (cit == caChannels_.end()) return;
    auto& ctx = cit->second;

    if (vr.isArray) {
        std::memcpy(vr.modPtr, data, vr.length);
    } else if (dbrType == DBR_DOUBLE) {
        double val = *static_cast<const double*>(data);
        if (val != ctx.lastPushedDouble) {
            std::ostringstream ss; ss << val;
            writeMemory(vr.modPtr, "double", ss.str());
        }
    } else {
        long val = *static_cast<const long*>(data);
        if (val != ctx.lastPushedInt) {
            writeMemory(vr.modPtr, "int", std::to_string(val));
        }
    }
}

// ─── Memory helpers ────────────────────────────────────────────

void PVServer::writeMemory(void* ptr, const std::string& type, const std::string& value)
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

std::string PVServer::readMemory(void* ptr, const std::string& type)
{
    if (!ptr) return "";
    if (type == "int") return std::to_string(*static_cast<int*>(ptr));
    if (type == "double") {
        std::ostringstream ss; ss << *static_cast<double*>(ptr); return ss.str();
    }
    if (type == "string") return *static_cast<std::string*>(ptr);
    return "";
}

// ─── Snapshot ──────────────────────────────────────────────────

bool PVServer::snapshot(const std::string& filepath)
{
    std::ofstream f(filepath, std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "[PVServer] snapshot: cannot open " << filepath << "\n";
        return false;
    }

    uint32_t count = 0;
    for (auto& rt : modules_) {
        if (rt.basePtr) count++;
    }
    f.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (auto& rt : modules_) {
        if (!rt.basePtr) continue;

        uint32_t nameLen = rt.name.size();
        f.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
        f.write(rt.name.data(), nameLen);

        uint32_t dataSize = rt.memorySize;
        f.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
        f.write(static_cast<const char*>(rt.basePtr), dataSize);
    }

    f.close();
    return true;
}

bool PVServer::restore(const std::string& filepath)
{
    std::ifstream f(filepath, std::ios::binary);
    if (!f.is_open()) {
        std::cout << "[PVServer] No snapshot to restore\n";
        return false;
    }

    uint32_t count;
    f.read(reinterpret_cast<char*>(&count), sizeof(count));

    for (uint32_t i = 0; i < count; i++) {
        uint32_t nameLen;
        f.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
        std::string name(nameLen, '\0');
        f.read(&name[0], nameLen);

        uint32_t dataSize;
        f.read(reinterpret_cast<char*>(&dataSize), sizeof(dataSize));

        for (auto& rt : modules_) {
            if (rt.name == name && rt.basePtr && rt.memorySize == dataSize) {
                f.read(static_cast<char*>(rt.basePtr), dataSize);
                std::cout << "[PVServer] Restored " << name << "\n";
                goto next;
            }
        }
        f.seekg(dataSize, std::ios::cur);
        next:;
    }

    f.close();
    return true;
}

// ─── generateDB ────────────────────────────────────────────────

bool PVServer::generateDB(const std::string& filepath)
{
    auto recInfo = [](const std::string& t, const std::string& dir, size_t nelm,
                      std::string& ftvl, size_t& outNelm) -> std::string {
        bool isArray = (t.size() > 4 && t.compare(t.size()-2, 2, "[]") == 0);
        std::string elem = isArray ? t.substr(0, t.size()-2) : t;

        if (isArray) {
            if (elem == "int") ftvl = "LONG";
            else if (elem == "double") ftvl = "DOUBLE";
            else if (elem == "string") ftvl = "STRING";
            else ftvl = "LONG";
            outNelm = nelm;
            return "waveform";
        }
        if (dir == "o") {
            if (elem == "int") return "longin";
            if (elem == "double") return "ai";
            if (elem == "string") return "stringin";
            return "longin";
        }
        if (elem == "int") return "longout";
        if (elem == "double") return "ao";
        if (elem == "string") return "stringout";
        if (elem == "enum") return "mbbo";
        return "longout";
    };

    std::ofstream f(filepath);
    if (!f.is_open()) return false;

    for (auto& cfg : config_) {
        for (auto& vc : cfg.vars) {
            if (!vc.epics) continue;
            std::string ftvl;
            size_t outNelm = 0;
            std::string rt = recInfo(vc.type, vc.dir, vc.nelm, ftvl, outNelm);
            f << "record(" << rt << ", \"" << vc.pvName << "\") {\n";
            f << "    field(DESC, \"" << cfg.name << ":" << vc.name << "\")\n";
            if (rt == "waveform") {
                f << "    field(FTVL, \"" << ftvl << "\")\n";
                f << "    field(NELM, \"" << outNelm << "\")\n";
            } else if (rt == "ao" || rt == "ai") {
                f << "    field(PREC, \"2\")\n";
            }
            f << "}\n\n";
        }
    }
    f.close();
    std::cout << "[PVServer] Generated " << filepath << "\n";
    return true;
}

void PVServer::enableIOC(const std::string& epicsBase, const std::string& dbPath)
{
    iocEpicsBase_ = epicsBase;
    iocDBPath_ = dbPath;
    iocEnabled_ = true;
}

} // namespace msm
