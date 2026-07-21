#include <thread>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <vector>
#include <atomic>
#include <signal.h>
#include "../sdk_include/CxDeviceCore.h"
#include "../sdk_include/CxDeviceInternal.h"
#include "../sdk_include/CxDefine.h"
#include "../extern/circular_buffer/user_circular_buffer.h"

static const int CHANNELS[] = {1, 2, 3, 4};
static const int NCH = 4;
static const size_t BATCH_FRAMES = 10000;

static std::atomic<bool> g_stop{false};
static void handle_signal(int) { g_stop.store(true); }

static uint32_t getTrigId(const uint8_t* p) {
    return ((uint32_t)p[17] << 24) | ((uint32_t)p[18] << 16) |
           ((uint32_t)p[19] << 8)  | (uint32_t)p[20];
}

static uint64_t getBunchId(const uint8_t* p, size_t flen) {
    const uint8_t* t = p + flen - 64;
    return ((uint64_t)t[2] << 40) | ((uint64_t)t[3] << 32) |
           ((uint64_t)t[4] << 24) | ((uint64_t)t[5] << 16) |
           ((uint64_t)t[6] << 8)  | (uint64_t)t[7];
}

struct ChanState {
    UserCircularBuffer ring{256 * 1024 * 1024, 384};
    std::atomic<size_t> frame_size{384};
    std::atomic<int64_t> total_frames{0};
    std::atomic<int64_t> total_batches{0};
};

static void consumerLoop(int ch, ChanState* s) {
    size_t batch = BATCH_FRAMES;
    while (!g_stop.load()) {
        while (!g_stop.load()) {
            if (s->ring.availableFrames() >= batch) break;
            std::this_thread::yield();
        }
        if (g_stop.load()) break;

        size_t flen = s->frame_size.load();
        size_t needed = batch * flen;
        std::vector<uint8_t> buf(needed);
        size_t got = s->ring.readData(buf.data(), needed);
        if (got != needed) continue;

        // Validate the first frame magic
        if (buf[0] != 0xEB || buf[1] != 0x91) continue;

        uint32_t tid = getTrigId(buf.data());
        uint64_t bid = getBunchId(buf.data(), flen);
        std::cout << "[ch" << ch << "] TrigID=" << tid
                  << " BunchID=" << bid << std::endl;

        s->total_batches.fetch_add(1);
    }
}

int main() {
    signal(SIGINT, handle_signal);

    CxDeviceSetSdkOption("WithLog", "false");
    CxDeviceSdkInit();

    CxChassisHandle chassis = nullptr;
    CxCardHandle* cards = nullptr;
    uint32_t card_count = 0;
    CxDeviceGetChassis(PXIe, &chassis);
    CxDeviceGetCards(chassis, &cards, &card_count);
    std::cout << "[main] total cards: " << card_count << "\n";

    CxCardHandle card = nullptr;
    for (uint32_t i = 0; i < card_count; i++) {
        uint32_t did = 0;
        CxDeviceGetCardDeviceId(cards[i], &did);
        std::cout << "[main] card " << i << " device=0x" << std::hex << did << std::dec << "\n";
        if (did == 0x1073) { card = cards[i]; }
    }
    if (!card) {
        std::cerr << "[main] No 1073 card found\n";
        CxDeviceSdkDeinit();
        return 1;
    }

    CxDeviceSetCard(card, "open");
    CxDeviceSetCardValue(card, "ClockSource", "0");
    CxDeviceSetCard(card, "init");
    CxDeviceSetCardValue(card, "SyncWindowEnable", "0");
    CxDeviceSetCardValue(card, "SyncSource", "0");

    for (int i = 0; i < NCH; i++) {
        int ch = CHANNELS[i];
        CxDeviceSetChannelValue(card, ch, "GlobalTrigEnable", "1");
        CxDeviceSetChannelValue(card, ch, "TrigSource", "4");
        CxDeviceSetChannelValue(card, ch, "TrigMode", "2");
        CxDeviceSetChannelValue(card, ch, "TrigLength", "128");
        CxDeviceSetChannelValue(card, ch, "TrigPulseWidth", "16");
        CxDeviceSetChannelValue(card, ch, "VoltageRange", "1000");
        CxDeviceSetChannelValue(card, ch, "TrigLevel", "200");
        std::cout << "[main] ch" << ch << " configured\n";
    }

    for (int i = 0; i < NCH; i++) {
        int ch = CHANNELS[i];
        CxDeviceSetChannelRecvBufferSize(card, ch, 16 * 1024 * 1024);
        CxDeviceStartChannelRecv(card, ch);
        CxDeviceSetChannel(card, ch, "run");
        std::cout << "[main] ch" << ch << " recv started\n";
    }
    std::cout << "[main] 4 channels started\n";

    ChanState states[NCH];
    std::thread consumers[NCH];
    for (int i = 0; i < NCH; i++)
        consumers[i] = std::thread(consumerLoop, CHANNELS[i], &states[i]);

    std::vector<uint8_t> buf(4 * 1024 * 1024);
    std::thread reader([&]() {
        while (!g_stop.load()) {
            for (int i = 0; i < NCH; i++) {
                int ch = CHANNELS[i];
                void* frames = nullptr;
                uint32_t n = 0;
                CxDeviceReadChannelFrames(card, ch, &frames, &n);
                if (n == 0) continue;

                auto* rp = static_cast<RawPack*>(frames);
                size_t flen = rp[0].BufferSize;
                if (flen == 0) continue;
                size_t bytes = (size_t)n * flen;

                if (bytes > buf.size()) buf.resize(bytes);
                uint8_t* dst = buf.data();
                for (uint32_t j = 0; j < n; j++) {
                    if (!rp[j].Buffer) { bytes = 0; break; }
                    std::memcpy(dst, rp[j].Buffer, flen);
                    dst += flen;
                }
                if (bytes == 0) continue;

                states[i].frame_size.store(flen);
                states[i].ring.setExpectedPacketSize(flen);
                size_t written = states[i].ring.writeFromDevice(buf.data(), bytes);
                states[i].total_frames.fetch_add(written / flen);
            }
        }
    });

    // Wait for first data
    auto wait_start = std::chrono::steady_clock::now();
    bool got_data = false;
    while (!g_stop.load()) {
        for (int i = 0; i < NCH; i++) {
            if (states[i].total_frames.load() > 0) got_data = true;
        }
        if (got_data) break;
        if (std::chrono::steady_clock::now() - wait_start > std::chrono::seconds(5)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!got_data) {
        std::cerr << "[main] no frames in 5s\n";
    } else {
        // Print stats every second
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - wait_start).count();
            std::cout << "[t=" << elapsed << "s]";
            for (int i = 0; i < NCH; i++)
                std::cout << "  ch" << CHANNELS[i] << "=" << states[i].total_frames.load();
            std::cout << std::endl;
        }
    }

    std::cout << "\n=== Results ===" << std::endl;
    g_stop.store(true);
    if (reader.joinable()) reader.join();
    for (int i = 0; i < NCH; i++) {
        if (consumers[i].joinable()) consumers[i].join();
    }

    for (int i = 0; i < NCH; i++) {
        CxDeviceStopChannelRecv(card, CHANNELS[i]);
        CxDeviceSetChannel(card, CHANNELS[i], "stop");
    }
    CxDeviceSetCard(card, "close");
    CxDeviceSdkDeinit();

    for (int i = 0; i < NCH; i++) {
        std::cout << "ch" << CHANNELS[i]
                  << ": frames=" << states[i].total_frames.load()
                  << " batches=" << states[i].total_batches.load()
                  << " overflow=" << states[i].ring.getOverflowCount()
                  << std::endl;
    }
    return got_data ? 0 : 1;
}
