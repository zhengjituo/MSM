#include "AddModule.h"
#include "MultiplyModule.h"
#include "WaveModule.h"
#include "IPCServer.h"
#include "PVServer.h"
#include "RESTServer.h"
#include "MSMGateway.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <vector>
#include <sstream>

using namespace msm;

static std::atomic<bool> g_running{true};

struct PeerArg {
    std::string name;
    std::string host;
    int repPort;
    int pubPort;
};

static void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "  --host <name>          Host identity (default: localhost)\n"
              << "  --rep-port <port>      REP mailbox port (default: 5555)\n"
              << "  --pub-port <port>      PUB broadcast port (default: 5556)\n"
              << "  --rest-port <port>     REST API port (default: 8080)\n"
              << "  --peer <name>,<host>,<rep-port>,<pub-port>\n"
              << "                         Add a peer (repeatable)\n"
              << "  -h, --help             Show this help\n";
}

class App {
public:
    App(const std::string& host, int repPort, int pubPort, int restPort, bool fast = false)
        : ipcServer_("/tmp/msm.sock")
        , restServer_(restPort)
        , gateway_()
        , hostName_(host)
        , repPort_(repPort)
        , pubPort_(pubPort)
        , restPort_(restPort)
        , fast_(fast) {}

    void addPeer(const std::string& name, const std::string& host, int repPort, int pubPort) {
        std::string repEp = host + ":" + std::to_string(repPort);
        gateway_.addPeer(name, repEp);
        peers_.push_back({name, host, repPort, pubPort});
    }

    void run() {
        MessageBus::instance().registerModule(&addModule_);
        MessageBus::instance().registerModule(&mulModule_);
        MessageBus::instance().registerModule(&waveModule_);
        if (!fast_) {
            MessageBus::instance().registerModule(&pvServer_);
        }
        MessageBus::instance().registerModule(&ipcServer_);
        MessageBus::instance().registerModule(&restServer_);
        MessageBus::instance().registerModule(&gateway_);

        addModule_.start();
        mulModule_.start();
        waveModule_.start();

        ipcServer_.loadConfig("msm_vars.json");
        restServer_.loadConfig("msm_vars.json");
        if (!fast_) {
            pvServer_.loadConfig("msm_vars.json");
        }

        auto setMem = [&](const std::string& name, Module& mod) {
            ipcServer_.setModuleMemory(name, mod.getVarBase(), mod.getVarSize());
            restServer_.setModuleMemory(name, mod.getVarBase(), mod.getVarSize());
            if (!fast_) {
                pvServer_.setModuleMemory(name, mod.getVarBase(), mod.getVarSize());
            }
        };
        setMem("AddModule", addModule_);
        setMem("MultiplyModule", mulModule_);
        setMem("WaveModule", waveModule_);

        restServer_.enableStaticDir(".");
        if (!fast_) {
            pvServer_.enableIOC("../module_core/extern/epics", "msm_pvs.db");
            pvServer_.start();
        }
        ipcServer_.start();
        gateway_.start();
        restServer_.start();

        std::cout << "\n========================================\n";
        std::cout << "  MSM Calculator - All Interfaces Ready\n";
        std::cout << "========================================\n";
        std::cout << "  Host:   " << hostName_ << "\n";
        std::cout << "  Config: msm_vars.json\n";
        std::cout << "  IPC:    /tmp/msm.sock\n";
        std::cout << "  PVA:    MSM:*\n";
        std::cout << "  Gateway REP:  tcp://*:" << repPort_ << "\n";
        std::cout << "  Gateway PUB:  tcp://*:" << pubPort_ << "\n";
        std::cout << "  REST:   http://localhost:" << restPort_ << "/api\n";
        std::cout << "  GUI:    http://localhost:" << restPort_ << "/\n";
        for (auto& p : peers_) {
            std::cout << "  Peer:   " << p.name << " (" << p.host
                      << " REP:" << p.repPort << " PUB:" << p.pubPort << ")\n";
        }
        std::cout << "========================================\n\n";

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        ipcServer_.stop();
        gateway_.stop();
        restServer_.stop();
        if (!fast_) pvServer_.stop();
        addModule_.stop();
        mulModule_.stop();
        waveModule_.stop();
        std::cout << "[Main] Shutdown complete\n";
    }

    void stop() { g_running = false; }

private:
    AddModule addModule_;
    MultiplyModule mulModule_;
    WaveModule waveModule_;
    PVServer pvServer_;
    IPCServer ipcServer_;
    RESTServer restServer_;
    MSMGateway gateway_;
    std::string hostName_;
    int repPort_, pubPort_, restPort_;
    bool fast_;
    std::vector<PeerArg> peers_;
};

static void handleSignal(int) {
    g_running = false;
}

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        parts.push_back(item);
    }
    return parts;
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    int repPort = 5555, pubPort = 5556, restPort = 8080;
    std::vector<PeerArg> peers;

    bool fast = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--rep-port" && i + 1 < argc) {
            repPort = std::stoi(argv[++i]);
        } else if (arg == "--pub-port" && i + 1 < argc) {
            pubPort = std::stoi(argv[++i]);
        } else if (arg == "--rest-port" && i + 1 < argc) {
            restPort = std::stoi(argv[++i]);
        } else if (arg == "--fast") {
            fast = true;
        } else if (arg == "--peer" && i + 1 < argc) {
            auto parts = split(argv[++i], ',');
            if (parts.size() == 4) {
                peers.push_back({parts[0], parts[1],
                                 std::stoi(parts[2]), std::stoi(parts[3])});
            } else {
                std::cerr << "Invalid --peer format. Use: name,host,rep-port,pub-port\n";
                return 1;
            }
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (fast) {
        std::cout << "[Fast mode] Skipping EPICS IOC\n";
    }

    App app(host, repPort, pubPort, restPort, fast);
    for (auto& p : peers) {
        app.addPeer(p.name, p.host, p.repPort, p.pubPort);
    }

    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    app.run();
    return 0;
}
