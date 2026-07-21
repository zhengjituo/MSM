#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include "../sdk_include/CxDeviceCore.h"
#include "../sdk_include/CxDeviceInternal.h"

static std::atomic<bool> g_stop{false};
static void handle_sig(int) { g_stop.store(true); }

int main() {
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    CxDeviceSetSdkOption("WithLog", "false");
    if (CxDeviceSdkInit() != 0) {
        std::cerr << "3010: SDK init failed\n"; return 1;
    }

    CxChassisHandle chassis = nullptr;
    CxCardHandle* cards = nullptr;
    uint32_t count = 0;
    if (CxDeviceGetChassis(PXIe, &chassis) != 0 ||
        CxDeviceGetCards(chassis, &cards, &count) != 0) {
        std::cerr << "3010: no chassis/cards\n"; return 1;
    }

    CxCardHandle card = nullptr;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t did = 0;
        CxDeviceGetCardDeviceId(cards[i], &did);
        if (did == 0x3010) { card = cards[i]; break; }
    }
    if (!card) { std::cerr << "3010: card not found\n"; return 1; }

    if (CxDeviceSetCard(card, "open") != 0) { std::cerr << "3010: open failed\n"; return 1; }
    if (CxDeviceSetCard(card, "init") != 0) { std::cerr << "3010: init failed\n"; return 1; }
    CxDeviceSetCardValue(card, "SyncTrigTrigMux", "2");
    CxDeviceSetCardValue(card, "SyncTrigGlbSync", "1");
    CxDeviceSetCardValue(card, "TrigSource", "2");
    CxDeviceSetCardValue(card, "SoftwareSync", "1");
    CxDeviceWriteReg(card, 0x1C020, 1);
    std::cout << "3010: trigger ENABLED (reg 0x1C020=1). Running..." << std::endl;

    while (!g_stop.load()) { std::this_thread::sleep_for(std::chrono::seconds(1)); }

    CxDeviceWriteReg(card, 0x1C020, 0);
    CxDeviceSetCard(card, "close");
    CxDeviceSdkDeinit();
    std::cout << "3010: cleaned up. Bye." << std::endl;
    return 0;
}
