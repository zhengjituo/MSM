// Capture 3 raw frames from 1073 card with 3010 trigger, dump to template.bin
#include "CxDeviceCore.h"
#include "CxDeviceInternal.h"
#include "CxDefine.h"
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>

int main() {
    CxDeviceSetSdkOption("WithLog", "false");
    CxDeviceSdkInit();

    CxChassisHandle chassis = nullptr;
    CxCardHandle* hwCards = nullptr;
    uint32_t hwCount = 0;
    CxDeviceGetChassis(PXIe, &chassis);
    CxDeviceGetCards(chassis, &hwCards, &hwCount);

    void* h1073 = nullptr;
    void* h3010 = nullptr;
    for (uint32_t i = 0; i < hwCount; i++) {
        uint32_t did = 0;
        CxDeviceGetCardDeviceId(hwCards[i], &did);
        if (did == 0x1073) h1073 = hwCards[i];
        else if (did == 0x3010) h3010 = hwCards[i];
    }
    if (!h1073) { fprintf(stderr, "No 1073 card\n"); return 1; }

    int ch = 1;
    CxDeviceSetCard(h1073, "open");
    CxDeviceSetCardValue(h1073, "ClockSource", "0");
    CxDeviceSetCard(h1073, "init");
    CxDeviceSetCardValue(h1073, "SyncWindowEnable", "0");
    CxDeviceSetCardValue(h1073, "SyncSource", "0");

    CxDeviceSetChannelValue(h1073, ch, "GlobalTrigEnable", "1");
    CxDeviceSetChannelValue(h1073, ch, "TrigSource", "global");
    CxDeviceSetChannelValue(h1073, ch, "TrigMode", "normal");
    CxDeviceSetChannelValue(h1073, ch, "TrigLength", "512");
    CxDeviceSetChannelValue(h1073, ch, "TrigPulseWidth", "16");
    CxDeviceSetChannelValue(h1073, ch, "VoltageRange", "1000");
    CxDeviceSetChannelValue(h1073, ch, "TrigLevel", "50");

    CxDeviceSetChannelRecvBufferSize(h1073, ch, 16 * 1024 * 1024);
    CxDeviceStartChannelRecv(h1073, ch);
    CxDeviceSetChannel(h1073, ch, "run");

    // Init 3010 trigger
    if (h3010) {
        CxDeviceSetCard(h3010, "open");
        CxDeviceSetCard(h3010, "init");
        CxDeviceSetCardValue(h3010, "SyncTrigTrigMux", "2");
        CxDeviceSetCardValue(h3010, "SyncTrigGlbSync", "1");
        CxDeviceSetCardValue(h3010, "TrigSource", "2");
        CxDeviceSetCardValue(h3010, "SoftwareSync", "1");
        CxDeviceWriteReg(h3010, 0x1C020, 1);  // trigger on
        fprintf(stderr, "3010 trigger enabled\n");
    }

    // Wait + poll
    void* frames = nullptr;
    uint32_t n = 0;
    for (int retry = 0; retry < 100; retry++) {
        CxDeviceReadChannelFrames(h1073, ch, &frames, &n);
        if (n > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    fprintf(stderr, "Captured %u frames\n", n);
    if (n == 0) {
        fprintf(stderr, "No frames after 10s\n");
        // clean up 3010
        if (h3010) {
            CxDeviceWriteReg(h3010, 0x1C020, 0);
            CxDeviceSetCard(h3010, "close");
        }
        CxDeviceStopChannelRecv(h1073, ch);
        CxDeviceSetChannel(h1073, ch, "stop");
        CxDeviceSetCard(h1073, "close");
        CxDeviceSdkDeinit();
        return 1;
    }

    auto* rp = static_cast<RawPack*>(frames);
    size_t flen = rp[0].BufferSize;
    fprintf(stderr, "Frame size: %zu bytes\n", flen);

    // Hex dump for template analysis
    fprintf(stderr, "\nFirst 64 bytes (header):\n");
    for (size_t i = 0; i < 64 && i < flen; i++) {
        fprintf(stderr, "%02x ", ((uint8_t*)rp[0].Buffer)[i]);
        if ((i + 1) % 16 == 0) fprintf(stderr, "\n");
    }
    if (flen >= 64) {
        fprintf(stderr, "\nLast 64 bytes (trailer):\n");
        for (size_t i = flen - 64; i < flen; i++) {
            fprintf(stderr, "%02x ", ((uint8_t*)rp[0].Buffer)[i]);
            if ((i + 1) % 16 == 0) fprintf(stderr, "\n");
        }
    }

    // Save 3 frames
    FILE* fp = fopen("template.bin", "wb");
    if (!fp) { perror("fopen"); return 1; }
    int save = (n < 3) ? (int)n : 3;
    for (int i = 0; i < save; i++) {
        if (rp[i].Buffer && rp[i].BufferSize == flen) {
            fwrite(rp[i].Buffer, 1, flen, fp);
            fprintf(stderr, "  saved frame[%d]: %u bytes\n", i, rp[i].BufferSize);
        }
    }
    fclose(fp);

    // Cleanup
    if (h3010) {
        CxDeviceWriteReg(h3010, 0x1C020, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CxDeviceSetCard(h3010, "close");
    }
    CxDeviceStopChannelRecv(h1073, ch);
    CxDeviceSetChannel(h1073, ch, "stop");
    CxDeviceSetCard(h1073, "close");
    CxDeviceSdkDeinit();
    return 0;
}
