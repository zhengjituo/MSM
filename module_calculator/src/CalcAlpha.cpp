#include "AddModule.h"
#include "MSMGateway.h"
#include "IPCServer.h"
#include "RESTServer.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

using namespace msm;

static std::atomic<bool> g_running{true};

int main(int argc, char* argv[]) {
    std::string configPath = std::string(BINARY_DIR) + "/msm_vars.json";

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if ((a == "-f" || a == "--config") && i + 1 < argc)
            configPath = argv[++i];
    }

    AddModule addMod;
    MSMGateway gw;
    gw.loadConfig(configPath);
    addMod.remotePeer_ = gw.firstPeerName();

    IPCServer ipc("/tmp/msm_alpha.sock");
    ipc.loadConfig(configPath);

    RESTServer rest(gw.restPort());
    rest.loadConfig(configPath);
    rest.enableStaticDir(std::string(BINARY_DIR));

    MessageBus::instance().registerModule(&addMod);
    MessageBus::instance().registerModule(&gw);
    MessageBus::instance().registerModule(&ipc);
    MessageBus::instance().registerModule(&rest);

    addMod.start();
    gw.start();
    ipc.start();
    rest.start();

    std::cout << "\n=== Calc Alpha (AddModule) ===\n"
              << "  Config: " << configPath << "\n"
              << "  REP: tcp://*:" << gw.repPort() << "\n"
              << "  PUB: tcp://*:" << gw.pubPort() << "\n"
              << "==============================\n\n";

    signal(SIGINT, [](int) { g_running = false; });
    signal(SIGTERM, [](int) { g_running = false; });

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    rest.stop();
    ipc.stop();
    gw.stop();
    addMod.stop();
    MessageBus::instance().unregisterModule("RESTServer");
    MessageBus::instance().unregisterModule("IPCServer");
    MessageBus::instance().unregisterModule("MSMGateway");
    MessageBus::instance().unregisterModule("AddModule");
    return 0;
}
