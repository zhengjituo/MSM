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
    snapshotInterval_ = j.value("snapshot_interval", snapshotInterval_);

    // Publish rate: accept rate_hz (preferred) or publish_interval_ms.
    double hz = pvRateHz_.load();
    if (j.contains("rate_hz")) {
        hz = j.value("rate_hz", hz);
    } else if (j.contains("publish_interval_ms")) {
        int ms = j.value("publish_interval_ms", 285);
        if (ms > 0) hz = 1000.0 / (double)ms;
    }
    setPublishRateHz(hz);

    if (!j.contains("modules")) {
        std::cout << "[PVServer] No modules in config (VarTable mode only)\n";
        return true;
    }

    for (auto& jmod : j["modules"]) {
        PVModuleConfig cfg;
        cfg.name = jmod["name"];
        cfg.memorySize = jmod.value("memory_size", 0);

        for (auto& jvar : jmod["variables"]) {
            PVVarConfig vc;
            vc.name = jvar["name"];
            vc.type = jvar["type"];
            vc.offset = jvar.value("offset", 0);
            vc.len = jvar.value("len", 4);
            vc.nelm = jvar.value("nelm", 0);
            vc.dir = jvar.value("dir", "io");
            vc.epics = true;
            if (jvar.contains("epics")) {
                auto& ef = jvar["epics"];
                if (ef.is_boolean()) vc.epics = ef.get<bool>();
                else vc.epics = ef.is_number_integer() && ef.get<int>() != 0;
            }
            vc.pvName = prefix_ + ":" + cfg.name + ":" + vc.name;
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
            vr.viaVarTable = false;

            varMap_[vc.pvName] = vr;
            std::cout << "[PVServer] PV " << vc.pvName
                      << " (" << vc.type << ", dir:" << vc.dir << ")\n";
        }
        break;
    }
}

void PVServer::attachVarTableModules()
{
    for (auto* mod : MessageBus::instance().modules()) {
        ModuleVars* vars = mod->getVars();
        if (!vars) continue;
        auto names = vars->getTypes();
        for (auto& [vname, vtype] : names) {
            std::string pvName = prefix_ + ":" + mod->getName() + ":" + vname;
            if (varMap_.count(pvName)) continue;

            PVVarRuntime vr;
            vr.moduleName = mod->getName();
            vr.varName = vname;
            vr.type = vtype;
            vr.dir = "io";
            vr.nelm = 1;
            vr.isArray = (vtype.size() > 2 && vtype.substr(vtype.size() - 2) == "[]");
            vr.viaVarTable = true;
            varMap_[pvName] = vr;
        }
        std::cout << "[PVServer] Attached " << mod->getName()
                  << " (" << vars->getTypes().size() << " vars, VarTable)\n";
    }
}

void PVServer::setPublishRateHz(double hz)
{
    if (hz < 2.0) hz = 2.0;
    if (hz > 10.0) hz = 10.0;
    pvRateHz_.store(hz);
}

std::string PVServer::readVar(PVVarRuntime& vr)
{
    if (vr.viaVarTable) {
        Module* mod = MessageBus::instance().findModule(vr.moduleName);
        if (!mod) return "";
        ModuleVars* vars = mod->getVars();
        if (!vars) return "";
        auto m = vars->list();
        auto it = m.find(vr.varName);
        return it != m.end() ? it->second : "";
    }
    return readMemory(vr.modPtr, vr.type);
}

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
        std::cout << "[PVServer] Waiting for IOC to start...\n";
        for (int i = 0; i < 20; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock >= 0) {
                struct sockaddr_in addr;
                memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_port = htons(5064);
                addr.sin_addr.s_addr = inet_addr("127.0.0.1");
                bool up = (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0);
                close(sock);
                if (up) break;
            }
        }
        if (!caInitChannels()) {
            std::cerr << "[PVServer] CA channel init failed\n";
        }

        // Publisher: throttled, change-only. Runs independently of the
        // message loop so modules never have to know about PV.
        publisherRunning_.store(true);
        publisherThread_ = std::thread([this]() { publisherLoop(); });
    }

    if (snapshotInterval_ > 0) {
        snapshotRunning_.store(true);
        snapshotThread_ = std::thread([this]() {
            while (snapshotRunning_.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(snapshotInterval_));
                if (!snapshotRunning_.load()) break;
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

    publisherRunning_.store(false);
    if (publisherThread_.joinable()) publisherThread_.join();

    snapshotRunning_.store(false);
    if (snapshotThread_.joinable()) snapshotThread_.join();

    caCleanup();
    killIOC();

    snapshot("msm_vars.snap");

    varMap_.clear();
    modules_.clear();
    config_.clear();

    Module::stop();
}

// ─── Publisher (throttled, change-only) ───────────────────────

void PVServer::publisherLoop()
{
    while (publisherRunning_.load()) {
        double hz = pvRateHz_.load();
        if (hz < 1e-6) hz = 3.5;
        int periodMs = (int)(1000.0 / hz);
        if (periodMs < 1) periodMs = 1;

        // Sleep in slices so stop() is responsive.
        int slept = 0;
        while (publisherRunning_.load() && slept < periodMs) {
            int slice = (periodMs - slept > 50) ? 50 : (periodMs - slept);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            slept += slice;
        }
        if (!publisherRunning_.load()) break;

        publishOnce(true);
    }
}

void PVServer::publishOnce(bool onlyChanged)
{
    if (!iocEnabled_ || iocPID_ <= 0) return;
    std::lock_guard<std::mutex> lock(pushMtx_);
    for (auto& [pvName, vr] : varMap_) {
        auto cit = caChannels_.find(pvName);
        if (cit == caChannels_.end()) continue;
        CAChannelContext& ctx = cit->second;
        if (!ctx.channel || !ctx.connected) continue;

        std::string val = readVar(vr);
        if (onlyChanged && vr.hasPushed && val == vr.lastPushed) continue;

        // Echo suppression: an external pvput just wrote this value in;
        // don't bounce it straight back out.
        if (ctx.suppressPending && val == ctx.suppressValue) {
            ctx.suppressPending = false;
            vr.lastPushed = val;
            vr.hasPushed = true;
            continue;
        }

        if (pushToIOC(pvName, vr)) {
            vr.lastPushed = val;
            vr.hasPushed = true;
        }
    }
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

    int connected = 0;
    for (int wait = 0; wait < 100 && connected < (int)caChannels_.size(); wait++) {
        ca_pend_event(0.1);
        connected = 0;
        for (auto& [pv, ctx] : caChannels_) {
            if (ctx.channel && ca_state(static_cast<chid>(ctx.channel)) == cs_conn)
                connected++;
        }
    }
    std::cout << "[PVServer] CA connected " << connected << "/" << n
              << " channels\n";

    subscribeAll();
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
        } else {
            delete user;
            ctx.subUser = nullptr;
        }
    }
    ca_flush_io();
}

void PVServer::caCleanup()
{
    for (auto& [pvName, ctx] : caChannels_) {
        if (ctx.subscription) ca_clear_subscription(static_cast<evid>(ctx.subscription));
        if (ctx.subUser) delete static_cast<SubUser*>(ctx.subUser);
        if (ctx.channel) ca_clear_channel(static_cast<chid>(ctx.channel));
    }
    caChannels_.clear();
    ca_context_destroy();
}


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
