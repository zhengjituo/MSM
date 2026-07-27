#pragma once
#include <cstdint>
#include <vector>
#include <cstddef>

static constexpr size_t HEADER_SIZE = 64;
static constexpr size_t TRAILER_SIZE = 64;
static constexpr size_t MIN_FRAME_SIZE = HEADER_SIZE + TRAILER_SIZE;

static constexpr double POW_20 = 1048576.0;
static constexpr double FFFF  = 65535.0;

struct FrameHeader {
    uint16_t magic;            // 0-1: 0xEB91
    uint32_t packet_length;    // 2-5
    uint8_t  packet_type;      // 6
    uint32_t board_type;       // 7-10
    uint16_t sample_rate;      // 11-12
    uint8_t  resolution;       // 13
    uint16_t channel_num;      // 14-15
    uint8_t  slot;             // 16
    uint32_t trig_id;          // 17-20
    uint64_t timestamp_sec;    // 21-26 (48 bits)
    uint64_t timestamp_ns;     // 27-31 (40 bits)
    uint8_t  fine_ts;          // 32
    uint32_t ext_window;       // 33-36
    uint32_t adc_scale;        // 37-40 (PreGain)
    uint16_t adc_offset;       // 43-44 (Offset, ref data_save_parameter_define.cpp:98 reads as unsigned)
    uint16_t adc_range;        // 47-48 (ref: line 99)
    uint8_t  adc_precision;    // 49
    uint8_t  adc_decode_mode;  // 50
    uint8_t  position;         // 51
    uint8_t  fw_version;       // 52
};

struct FrameTrailer {
    uint16_t magic;            // 0-1: 0xEB92
    uint64_t bunch_id;         // 2-7 (48 bits)
    int16_t  min_raw;          // 8-9
    int16_t  max_raw;          // 10-11
    int32_t  sum_l;            // 12-15
    int32_t  sum_h;            // 16-19
    uint16_t vpp_raw;          // 20-21
    // 22-23: reserved (per data_save_parameter_define.cpp:124)
    int64_t  x_raw;            // 24-31 (ref: line 125)
    int64_t  y_raw;            // 32-39 (ref: line 127)
    int64_t  i_raw;            // 40-47 (ref: line 130)
};

struct CalibratedFrame {
    double bunch_id;
    double min_val;
    double max_val;
    double sum_l;
    double sum_h;
    double vpp;
    double x;
    double y;
    double i;
};

bool parseHeader(const uint8_t* data, size_t len, FrameHeader& hdr);
bool parseTrailer(const uint8_t* data, size_t len, FrameTrailer& tlr);
CalibratedFrame calibrate(const FrameHeader& hdr, const FrameTrailer& tlr);
std::vector<double> extractWaveform(const uint8_t* data, size_t len, const FrameHeader& hdr);

// Lower-level helpers
inline uint16_t readU16BE(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] << 8) | (uint16_t)p[1];
}
inline int16_t readI16BE(const uint8_t* p) {
    return (int16_t)((uint16_t)p[0] << 8) | (uint16_t)p[1];
}
inline uint32_t readU32BE(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
inline int32_t readI32BE(const uint8_t* p) {
    return (int32_t)readU32BE(p);
}
inline uint64_t readU48BE(const uint8_t* p) {
    return ((uint64_t)p[0] << 40) | ((uint64_t)p[1] << 32) |
           ((uint64_t)p[2] << 24) | ((uint64_t)p[3] << 16) |
           ((uint64_t)p[4] << 8)  | (uint64_t)p[5];
}
inline uint64_t readU64BE(const uint8_t* p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8)  | (uint64_t)p[7];
}
inline int64_t readI64BE(const uint8_t* p) {
    return (int64_t)readU64BE(p);
}
