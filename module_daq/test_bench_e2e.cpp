#include <iostream>
#include <vector>
#include <cmath>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <thread>

#define CPPHTTPLIB_OPENSSL_SUPPORT 0
#include "../module_core/extern/include/httplib.h"

static constexpr size_t HEADER_SIZE = 64;
static constexpr size_t TRAILER_SIZE = 64;
static constexpr size_t WAVE_SIZE = 1024;
static constexpr size_t FRAME_SIZE = HEADER_SIZE + WAVE_SIZE + TRAILER_SIZE;
static constexpr size_t BATCH_SIZE = 10000;
static constexpr size_t BYTES_PER_CH = BATCH_SIZE * FRAME_SIZE;
static constexpr size_t NUM_CH = 4;

// Binary protocol header
#pragma pack(push, 1)
struct BatchHdr {
    char magic[4];      // "MSM1"
    uint16_t hdrLen;
    uint16_t numCh;
    uint32_t batchSize;
    uint64_t sentSeq;
    uint64_t tUs;
};
struct VarDesc {
    uint8_t type;       // 0=U32, 1=U64, 2=DBL
    uint8_t nameLen;
    // char name[nameLen];
    // data follows: type[batchSize]
};
#pragma pack(pop)

static inline uint32_t readU32BE(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static inline uint64_t readU48BE(const uint8_t* p) {
    return ((uint64_t)p[0] << 40) | ((uint64_t)p[1] << 32) |
           ((uint64_t)p[2] << 24) | ((uint64_t)p[3] << 16) |
           ((uint64_t)p[4] << 8)  | (uint64_t)p[5];
}

static inline void writeU32BE(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;  p[3] = v & 0xFF;
}
static inline void writeDoubleBE(uint8_t* p, double v) {
    uint64_t bits; memcpy(&bits, &v, 8);
    p[0] = (bits >> 56) & 0xFF; p[1] = (bits >> 48) & 0xFF;
    p[2] = (bits >> 40) & 0xFF; p[3] = (bits >> 32) & 0xFF;
    p[4] = (bits >> 24) & 0xFF; p[5] = (bits >> 16) & 0xFF;
    p[6] = (bits >> 8) & 0xFF;  p[7] = bits & 0xFF;
}
static inline double readDoubleBE(const uint8_t* p) {
    uint64_t bits = ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
                    ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
                    ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
                    ((uint64_t)p[6] << 8)  | (uint64_t)p[7];
    double v; memcpy(&v, &bits, 8); return v;
}

void fillFrames(uint8_t* buf, uint64_t trigBase, uint64_t bunchBase,
                int channel, uint64_t seq = 0) {
    for (size_t f = 0; f < BATCH_SIZE; f++) {
        uint8_t* frame = buf + f * FRAME_SIZE;
        frame[0] = 0xEB; frame[1] = 0x91;
        uint64_t tid = trigBase + f;
        frame[17] = (tid >> 24) & 0xFF; frame[18] = (tid >> 16) & 0xFF;
        frame[19] = (tid >> 8) & 0xFF;  frame[20] = tid & 0xFF;
        uint8_t* trailer = frame + HEADER_SIZE + WAVE_SIZE;
        trailer[0] = 0xEB; trailer[1] = 0x92;
        uint64_t bid = bunchBase + f * 10;
        trailer[2] = (bid >> 40) & 0xFF; trailer[3] = (bid >> 32) & 0xFF;
        trailer[4] = (bid >> 24) & 0xFF; trailer[5] = (bid >> 16) & 0xFF;
        trailer[6] = (bid >> 8) & 0xFF;  trailer[7] = bid & 0xFF;
        // sum: (ch+1)*10 + 正弦缓变 ±5, seq 使每批略有偏移
        uint32_t sum = (uint32_t)((channel + 1) * 10) + 5
                     + (uint32_t)(5.0 * sin(f * 0.0005 + channel + seq * 0.01));
        writeU32BE(trailer + 8, sum);
        // Channel 1 only: X, Y at offset 24/32 (DBL BE)
        if (channel == 0) {
            double xPos = 0.5 + 0.4 * sin(f * 0.0003 + seq * 0.005);
            double yPos = 0.5 + 0.4 * cos(f * 0.00035 + seq * 0.005);
            writeDoubleBE(trailer + 24, xPos);
            writeDoubleBE(trailer + 32, yPos);
        }
    }
}

void extract(const uint8_t* buf, uint32_t* trigIds, uint64_t* bunchIds,
             uint32_t* sums, double* xPos, double* yPos, double* intensity) {
    for (size_t f = 0; f < BATCH_SIZE; f++) {
        const uint8_t* frame = buf + f * FRAME_SIZE;
        trigIds[f] = readU32BE(frame + 17);
        const uint8_t* trailer = frame + HEADER_SIZE + WAVE_SIZE;
        bunchIds[f] = readU48BE(trailer + 2);
        sums[f] = readU32BE(trailer + 8);
        xPos[f] = readDoubleBE(trailer + 24);
        yPos[f] = readDoubleBE(trailer + 32);
        intensity[f] = readDoubleBE(trailer + 40);
    }
}

// Build binary packet: header + per-channel numVars + var desc + raw data
std::vector<uint8_t> buildBinary(const std::vector<uint32_t*>& trig,
                                  const std::vector<uint64_t*>& bunch,
                                  const std::vector<uint32_t*>& sums,
                                  const std::vector<double*>& xPos,
                                  const std::vector<double*>& yPos,
                                  const std::vector<double*>& intensity,
                                  uint64_t sentSeq, uint64_t tUs) {
    // Per-channel variable config: ch0=6 vars, ch1-3=3 vars
    std::vector<std::vector<std::string>> chVarNames = {
        {"trig_id", "bunch_id", "sum", "x_pos", "y_pos", "intensity"},
        {"trig_id", "bunch_id", "sum"},
        {"trig_id", "bunch_id", "sum"},
        {"trig_id", "bunch_id", "sum"}
    };
    std::vector<std::vector<uint8_t>> chVarTypes = {
        {0, 1, 0, 2, 2, 2},
        {0, 1, 0},
        {0, 1, 0},
        {0, 1, 0}
    };
    std::vector<size_t> varSizes6 = {
        sizeof(uint32_t) * BATCH_SIZE,
        sizeof(uint64_t) * BATCH_SIZE,
        sizeof(uint32_t) * BATCH_SIZE,
        sizeof(double) * BATCH_SIZE,
        sizeof(double) * BATCH_SIZE,
        sizeof(double) * BATCH_SIZE
    };
    std::vector<size_t> varSizes3 = {varSizes6[0], varSizes6[1], varSizes6[2]};
    std::vector<std::vector<size_t>> chVarSizes = {varSizes6, varSizes3, varSizes3, varSizes3};

    size_t totalSize = sizeof(BatchHdr);
    for (size_t ch = 0; ch < NUM_CH; ch++) {
        totalSize += sizeof(uint16_t);
        for (size_t v = 0; v < chVarNames[ch].size(); v++)
            totalSize += sizeof(VarDesc) + chVarNames[ch][v].size() + chVarSizes[ch][v];
    }

    std::vector<uint8_t> pkt(totalSize);
    size_t off = 0;

    BatchHdr hdr;
    memcpy(hdr.magic, "MSM1", 4);
    hdr.hdrLen = sizeof(BatchHdr);
    hdr.numCh = NUM_CH;
    hdr.batchSize = BATCH_SIZE;
    hdr.sentSeq = sentSeq;
    hdr.tUs = tUs;
    memcpy(&pkt[off], &hdr, sizeof(hdr)); off += sizeof(hdr);

    std::vector<const void*> dataPtrs = {nullptr, nullptr, nullptr,
                                         nullptr, nullptr, nullptr};
    for (size_t ch = 0; ch < NUM_CH; ch++) {
        dataPtrs[0] = trig[ch];
        dataPtrs[1] = bunch[ch];
        dataPtrs[2] = sums[ch];
        dataPtrs[3] = xPos[ch];
        dataPtrs[4] = yPos[ch];
        dataPtrs[5] = intensity[ch];

        uint16_t nv = (uint16_t)chVarNames[ch].size();
        memcpy(&pkt[off], &nv, sizeof(nv)); off += sizeof(nv);

        for (size_t v = 0; v < nv; v++) {
            VarDesc vd;
            vd.type = chVarTypes[ch][v]; vd.nameLen = chVarNames[ch][v].size();
            memcpy(&pkt[off], &vd, sizeof(vd)); off += sizeof(vd);
            memcpy(&pkt[off], chVarNames[ch][v].data(), chVarNames[ch][v].size()); off += chVarNames[ch][v].size();
            memcpy(&pkt[off], dataPtrs[v], chVarSizes[ch][v]); off += chVarSizes[ch][v];
        }
    }
    return pkt;
}

int main() {
    std::cout << "=== MSM DAQ E2E Binary Test: " << NUM_CH << "ch ===" << std::endl;

    std::vector<uint8_t*> bufs(NUM_CH);
    std::vector<uint32_t*> trigIds(NUM_CH);
    std::vector<uint64_t*> bunchIds(NUM_CH);
    std::vector<uint32_t*> sums(NUM_CH);
    std::vector<double*> xPos(NUM_CH);
    std::vector<double*> yPos(NUM_CH);
    std::vector<double*> intensity(NUM_CH);
    for (size_t ch = 0; ch < NUM_CH; ch++) {
        bufs[ch] = new uint8_t[BYTES_PER_CH];
        trigIds[ch] = new uint32_t[BATCH_SIZE];
        bunchIds[ch] = new uint64_t[BATCH_SIZE];
        sums[ch] = new uint32_t[BATCH_SIZE];
        xPos[ch] = new double[BATCH_SIZE];
        yPos[ch] = new double[BATCH_SIZE];
        intensity[ch] = new double[BATCH_SIZE];
    }

    fillFrames(bufs[0], 1000000, 10000000, 0);
    extract(bufs[0], trigIds[0], bunchIds[0], sums[0], xPos[0], yPos[0], intensity[0]);
    bool trigOk = true, bunchOk = true, sumOk = true;
    for (size_t f = 0; f < BATCH_SIZE; f++) {
        if (trigIds[0][f] != 1000000 + f) trigOk = false;
        if (bunchIds[0][f] != 10000000 + f * 10) bunchOk = false;
        if (sums[0][f] != (uint32_t)(10 + 5 + 5.0 * sin(f * 0.0005))) sumOk = false;
    }
    std::cout << "  Verify: TrigID=" << (trigOk ? "✓" : "✗")
              << "  BunchID=" << (bunchOk ? "✓" : "✗")
              << "  sum=" << (sumOk ? "✓" : "✗") << std::endl;
    std::cout << "  sum[0]=" << sums[0][0] << "  sum[9999]=" << sums[0][9999] << std::endl;
    std::cout << "  ch0: x_pos[0]=" << xPos[0][0] << "  y_pos[0]=" << yPos[0][0]
              << "  intensity[0]=" << intensity[0][0] << std::endl;

    auto bin = buildBinary(trigIds, bunchIds, sums, xPos, yPos, intensity, 0, 0);
    std::cout << "  Binary size: " << bin.size() << "B ("
              << bin.size() / 1024 << "KB)" << std::endl;

    httplib::Client cli("localhost", 8088);
    cli.set_connection_timeout(1);
    cli.set_read_timeout(2);

    // Warmup
    for (int r = 0; r < 5; r++) {
        for (size_t ch = 0; ch < NUM_CH; ch++) fillFrames(bufs[ch], 1000000, 10000000, (int)ch);
        for (size_t ch = 0; ch < NUM_CH; ch++) extract(bufs[ch], trigIds[ch], bunchIds[ch], sums[ch], xPos[ch], yPos[ch], intensity[ch]);
        auto bin = buildBinary(trigIds, bunchIds, sums, xPos, yPos, intensity, 0, 0);
        cli.Post("/api/data", (const char*)bin.data(), bin.size(), "application/octet-stream");
    }

    // Timing
    std::cout << "\n=== E2E Binary Timing ===" << std::endl;
    const int RUNS = 100;
    double min = 1e9, max = 0, avg = 0;
    int okCnt = 0;
    for (int r = 0; r < RUNS; r++) {
        auto t0 = std::chrono::steady_clock::now();
        for (size_t ch = 0; ch < NUM_CH; ch++) fillFrames(bufs[ch], 1000000, 10000000, (int)ch);
        for (size_t ch = 0; ch < NUM_CH; ch++) extract(bufs[ch], trigIds[ch], bunchIds[ch], sums[ch], xPos[ch], yPos[ch], intensity[ch]);
        auto t1 = std::chrono::steady_clock::now();
        auto bin = buildBinary(trigIds, bunchIds, sums, xPos, yPos, intensity, 0, 0);
        auto t2 = std::chrono::steady_clock::now();
        auto res = cli.Post("/api/data", (const char*)bin.data(), bin.size(), "application/octet-stream");
        auto t3 = std::chrono::steady_clock::now();

        double eUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
        double sUs = std::chrono::duration<double, std::micro>(t2 - t1).count();
        double pUs = std::chrono::duration<double, std::micro>(t3 - t2).count();
        double total = eUs + sUs + pUs;

        if (res && res->status == 200) {
            okCnt++; avg += total;
            if (total < min) min = total;
            if (total > max) max = total;
        }
        if (r % 20 == 0)
            std::cout << "  run " << r << ": extract=" << (int)eUs
                      << "us  serialize=" << (int)sUs << "us  post=" << (int)pUs
                      << "us  total=" << (int)total << "us" << std::endl;
    }
    avg /= okCnt;
    std::cout << "\n=== Summary ===" << std::endl;
    std::cout << "  Binary size: " << (bin.size() / 1024) << "KB" << std::endl;
    std::cout << "  Per batch:   min=" << (int)min << "us  avg=" << (int)avg
              << "us  max=" << (int)max << "us" << std::endl;

    // Live loop
    std::cout << "\n=== Live loop ===" << std::endl;
    int seq = 0;
    uint64_t trigBase = 1000000, bunchBase = 10000000;
    while (true) {
        for (size_t ch = 0; ch < NUM_CH; ch++) fillFrames(bufs[ch], trigBase, bunchBase, (int)ch, seq);
        trigBase += BATCH_SIZE;  // uint64_t, 写帧时截断为32位
        bunchBase += BATCH_SIZE * 10;
        auto tElapsed = std::chrono::steady_clock::now();
        for (size_t ch = 0; ch < NUM_CH; ch++) extract(bufs[ch], trigIds[ch], bunchIds[ch], sums[ch], xPos[ch], yPos[ch], intensity[ch]);
        // I = (sum1 + sum2 + sum3 + sum4) / 4
        for (size_t f = 0; f < BATCH_SIZE; f++)
            intensity[0][f] = (double)(sums[0][f] + sums[1][f] + sums[2][f] + sums[3][f]) / 4.0;
        auto bin = buildBinary(trigIds, bunchIds, sums, xPos, yPos, intensity, seq++,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        auto res = cli.Post("/api/data", (const char*)bin.data(), bin.size(), "application/octet-stream");
        auto elapsed = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - tElapsed).count();
        std::cout << "  batch=" << seq-1 << "  " << (int)elapsed << "us"
                  << (res && res->status == 200 ? " ✓" : " ✗") << std::endl;
    }

    for (size_t ch = 0; ch < NUM_CH; ch++) {
        delete[] bufs[ch]; delete[] trigIds[ch]; delete[] bunchIds[ch];
        delete[] sums[ch]; delete[] xPos[ch]; delete[] yPos[ch]; delete[] intensity[ch];
    }
    return 0;
}
