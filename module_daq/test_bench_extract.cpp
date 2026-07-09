#include <iostream>
#include <vector>
#include <chrono>
#include <cstring>
#include <cstdint>

// Frame layout: [64B header][1024B waveform][64B trailer] = 1152B
static constexpr size_t HEADER_SIZE = 64;
static constexpr size_t TRAILER_SIZE = 64;
static constexpr size_t WAVE_SIZE = 1024; // 512 U16
static constexpr size_t FRAME_SIZE = HEADER_SIZE + WAVE_SIZE + TRAILER_SIZE;
static constexpr size_t BATCH_SIZE = 10000;
static constexpr size_t TOTAL_BYTES = BATCH_SIZE * FRAME_SIZE;

static inline uint32_t readU32BE(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline uint64_t readU48BE(const uint8_t* p) {
    return ((uint64_t)p[0] << 40) | ((uint64_t)p[1] << 32) |
           ((uint64_t)p[2] << 24) | ((uint64_t)p[3] << 16) |
           ((uint64_t)p[4] << 8)  | (uint64_t)p[5];
}

void fillFrames(uint8_t* buf) {
    for (size_t f = 0; f < BATCH_SIZE; f++) {
        uint8_t* frame = buf + f * FRAME_SIZE;
        // Header sync
        frame[0] = 0xEB; frame[1] = 0x91;
        // TrigID
        uint64_t tid = 1000000 + f * 10000;
        frame[17] = (tid >> 24) & 0xFF; frame[18] = (tid >> 16) & 0xFF;
        frame[19] = (tid >> 8) & 0xFF;  frame[20] = tid & 0xFF;
        // Waveform (progressive U16)
        uint16_t* wave = (uint16_t*)(frame + HEADER_SIZE);
        for (int s = 0; s < 512; s++) wave[s] = s + 1;
        // Trailer sync
        uint8_t* trailer = frame + HEADER_SIZE + WAVE_SIZE;
        trailer[0] = 0xEB; trailer[1] = 0x92;
        // BunchID
        uint64_t bid = 10000000 + f * 100000;
        trailer[2] = (bid >> 40) & 0xFF; trailer[3] = (bid >> 32) & 0xFF;
        trailer[4] = (bid >> 24) & 0xFF; trailer[5] = (bid >> 16) & 0xFF;
        trailer[6] = (bid >> 8) & 0xFF;  trailer[7] = bid & 0xFF;
    }
}

// Method 1: Basic for-loop
void extractBasic(const uint8_t* buf, uint32_t* trigIds, uint64_t* bunchIds) {
    for (size_t f = 0; f < BATCH_SIZE; f++) {
        const uint8_t* frame = buf + f * FRAME_SIZE;
        trigIds[f] = readU32BE(frame + 17);
        bunchIds[f] = readU48BE(frame + HEADER_SIZE + WAVE_SIZE + 2);
    }
}

// Method 2: Pointer-increment (no multiply per iteration)
void extractPtrInc(const uint8_t* buf, uint32_t* trigIds, uint64_t* bunchIds) {
    const uint8_t* p = buf;
    for (size_t f = 0; f < BATCH_SIZE; f++) {
        trigIds[f] = readU32BE(p + 17);
        bunchIds[f] = readU48BE(p + HEADER_SIZE + WAVE_SIZE + 2);
        p += FRAME_SIZE;
    }
}

// Method 3: Manual prefetch hint
void extractPrefetch(const uint8_t* buf, uint32_t* trigIds, uint64_t* bunchIds) {
    const uint8_t* p = buf;
    for (size_t f = 0; f < BATCH_SIZE; f++) {
        __builtin_prefetch(p + FRAME_SIZE, 0, 0);
        trigIds[f] = readU32BE(p + 17);
        bunchIds[f] = readU48BE(p + HEADER_SIZE + WAVE_SIZE + 2);
        p += FRAME_SIZE;
    }
}

// Method 4: memcpy-based unaligned read (avoids shift operations)
static inline uint32_t readU32BE_memcpy(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}
static inline uint64_t readU48BE_memcpy(const uint8_t* p) {
    uint64_t v = 0;
    memcpy(&v, p, 6);
    return __builtin_bswap64(v) >> 16;
}

void extractMemcpy(const uint8_t* buf, uint32_t* trigIds, uint64_t* bunchIds) {
    const uint8_t* p = buf;
    for (size_t f = 0; f < BATCH_SIZE; f++) {
        trigIds[f] = readU32BE_memcpy(p + 17);
        bunchIds[f] = readU48BE_memcpy(p + HEADER_SIZE + WAVE_SIZE + 2);
        p += FRAME_SIZE;
    }
}

// Method 5: memcpy + prefetch
void extractMemcpyPrefetch(const uint8_t* buf, uint32_t* trigIds, uint64_t* bunchIds) {
    const uint8_t* p = buf;
    for (size_t f = 0; f < BATCH_SIZE; f++) {
        __builtin_prefetch(p + FRAME_SIZE, 0, 0);
        trigIds[f] = readU32BE_memcpy(p + 17);
        bunchIds[f] = readU48BE_memcpy(p + HEADER_SIZE + WAVE_SIZE + 2);
        p += FRAME_SIZE;
    }
}

// Method 6: SIMD-like bulk (process 4 frames at once with pointer increment)
void extractBulk4(const uint8_t* buf, uint32_t* trigIds, uint64_t* bunchIds) {
    const uint8_t* p = buf;
    size_t f = 0;
    for (; f + 3 < BATCH_SIZE; f += 4) {
        trigIds[f]   = readU32BE(p + 17);
        bunchIds[f]   = readU48BE(p + HEADER_SIZE + WAVE_SIZE + 2);
        trigIds[f+1] = readU32BE(p + FRAME_SIZE + 17);
        bunchIds[f+1] = readU48BE(p + FRAME_SIZE + HEADER_SIZE + WAVE_SIZE + 2);
        trigIds[f+2] = readU32BE(p + FRAME_SIZE*2 + 17);
        bunchIds[f+2] = readU48BE(p + FRAME_SIZE*2 + HEADER_SIZE + WAVE_SIZE + 2);
        trigIds[f+3] = readU32BE(p + FRAME_SIZE*3 + 17);
        bunchIds[f+3] = readU48BE(p + FRAME_SIZE*3 + HEADER_SIZE + WAVE_SIZE + 2);
        p += FRAME_SIZE * 4;
    }
    for (; f < BATCH_SIZE; f++) {
        trigIds[f] = readU32BE(p + 17);
        bunchIds[f] = readU48BE(p + HEADER_SIZE + WAVE_SIZE + 2);
        p += FRAME_SIZE;
    }
}

double bench(const char* label, void (*func)(const uint8_t*, uint32_t*, uint64_t*),
             const uint8_t* buf, uint32_t* trigIds, uint64_t* bunchIds, int runs)
{
    // warmup
    func(buf, trigIds, bunchIds);

    auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < runs; r++)
        func(buf, trigIds, bunchIds);
    auto t1 = std::chrono::steady_clock::now();

    double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / runs;
    // verify
    uint32_t expectedTrig = (uint32_t)(1000000);
    uint64_t expectedBunch = 10000000ULL;
    bool ok = (trigIds[0] == expectedTrig && bunchIds[0] == expectedBunch);
    std::cout << "  " << label << ": " << us << " us"
              << "  verify=" << (ok ? "OK" : "FAIL")
              << "  trigs[" << BATCH_SIZE-1 << "]=" << trigIds[BATCH_SIZE-1]
              << std::endl;
    return us;
}

int main() {
    auto* buf = new uint8_t[TOTAL_BYTES];
    fillFrames(buf);

    auto* trigIds = new uint32_t[BATCH_SIZE];
    auto* bunchIds = new uint64_t[BATCH_SIZE];

    const int RUNS = 1000;

    std::cout << "Batch: " << BATCH_SIZE << " frames x "
              << FRAME_SIZE << "B = " << (TOTAL_BYTES / 1024 / 1024.0) << " MB"
              << std::endl;

    // Single batch (10k frames)
    std::cout << "\n--- Single batch ---" << std::endl;
    bench("basic for-loop",            extractBasic,        buf, trigIds, bunchIds, RUNS);
    bench("pointer increment",         extractPtrInc,       buf, trigIds, bunchIds, RUNS);
    bench("prefetch",                  extractPrefetch,     buf, trigIds, bunchIds, RUNS);
    bench("memcpy bswap",              extractMemcpy,       buf, trigIds, bunchIds, RUNS);
    bench("memcpy+prefetch",           extractMemcpyPrefetch,buf, trigIds, bunchIds, RUNS);
    bench("bulk4",                     extractBulk4,        buf, trigIds, bunchIds, RUNS);

    // Estimate for 56 batches (as in previous full-run scenario)
    std::cout << "\n--- Estimated for 56 batches (" << (BATCH_SIZE * 56) << " frames) ---" << std::endl;
    double single = bench("pointer increment (56x)",  extractPtrInc, buf, trigIds, bunchIds, RUNS);
    std::cout << "  56 batches total: " << (single * 56 / 1000) << " ms" << std::endl;

    delete[] buf;
    delete[] trigIds;
    delete[] bunchIds;
    return 0;
}
