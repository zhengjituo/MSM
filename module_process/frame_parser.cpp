#include "frame_parser.h"
#include <cstring>
#include <cmath>

static constexpr double TWO_POW_32 = 4294967296.0;

bool parseHeader(const uint8_t* data, size_t len, FrameHeader& hdr) {
    if (len < HEADER_SIZE) return false;
    if (data[0] != 0xEB || data[1] != 0x91) return false;

    hdr.magic          = 0xEB91;
    hdr.packet_length  = readU32BE(data + 2);
    hdr.packet_type    = data[6];
    hdr.board_type     = readU32BE(data + 7);
    hdr.sample_rate    = readU16BE(data + 11);
    hdr.resolution     = data[13];
    hdr.channel_num    = readU16BE(data + 14);
    hdr.slot           = data[16];
    hdr.trig_id        = readU32BE(data + 17);
    hdr.timestamp_sec  = readU48BE(data + 21);
    hdr.timestamp_ns   = ((uint64_t)data[27] << 32) |
                         ((uint64_t)data[28] << 24) |
                         ((uint64_t)data[29] << 16) |
                         ((uint64_t)data[30] << 8)  | data[31];
    hdr.fine_ts        = data[32];
    hdr.ext_window     = readU32BE(data + 33);
    hdr.adc_scale      = readU32BE(data + 37);      // PreGain (ref: line 97)
    hdr.adc_offset     = readU16BE(data + 43);       // Offset: 2B @ 43-44 (ref: line 98)
    hdr.adc_range      = readU16BE(data + 47);       // Range: 2B @ 47-48 (ref: line 99)
    hdr.adc_precision  = data[49];
    hdr.adc_decode_mode = data[50];
    hdr.position       = data[51];
    hdr.fw_version     = data[52];

    return true;
}

bool parseTrailer(const uint8_t* data, size_t len, FrameTrailer& tlr) {
    if (len < TRAILER_SIZE) return false;
    if (data[0] != 0xEB || data[1] != 0x92) return false;

    tlr.magic    = 0xEB92;
    tlr.bunch_id = readU48BE(data + 2);
    tlr.min_raw  = readI16BE(data + 8);
    tlr.max_raw  = readI16BE(data + 10);
    tlr.sum_l    = readI32BE(data + 12);
    tlr.sum_h    = readI32BE(data + 16);
    tlr.vpp_raw  = readU16BE(data + 20);
    // 22-23: reserved (skip)
    tlr.x_raw    = readI64BE(data + 24);   // ref: line 125
    tlr.y_raw    = readI64BE(data + 32);   // ref: line 127
    tlr.i_raw    = readI64BE(data + 40);   // ref: line 130

    return true;
}

CalibratedFrame calibrate(const FrameHeader& hdr, const FrameTrailer& tlr) {
    CalibratedFrame cal = {};

    double scale = hdr.adc_scale / POW_20;
    double range_factor = hdr.adc_range / FFFF;

    // bunchId: raw value, no calibration
    cal.bunch_id = (double)tlr.bunch_id;

    // Waveform-derived features calibrated with scale * range_factor
    auto applyCal = [&](double raw) -> double {
        return (raw * scale) * range_factor;
    };

    cal.min_val = applyCal(tlr.min_raw);
    cal.max_val = applyCal(tlr.max_raw);
    cal.sum_l   = applyCal(tlr.sum_l);
    cal.sum_h   = applyCal(tlr.sum_h);
    cal.vpp     = applyCal(tlr.vpp_raw);

    // X/Y: int64 / 2^32, NO PreGain/Range calibration (ref data_save_parameter_define.cpp:125-128)
    cal.x = (double)(int64_t)tlr.x_raw / TWO_POW_32;
    cal.y = (double)(int64_t)tlr.y_raw / TWO_POW_32;

    // I: int64 / 2^32 * 2.0, THEN * PreGain / 2^20 * Range / 0xFFFF (ref: lines 130-131)
    double lightIntensity = (double)(int64_t)tlr.i_raw / TWO_POW_32 * 2.0;
    cal.i = lightIntensity * scale * range_factor;

    return cal;
}

std::vector<double> extractWaveform(const uint8_t* data, size_t len, const FrameHeader& hdr) {
    std::vector<double> result;

    size_t trig_length = 0;
    if (hdr.packet_length >= HEADER_SIZE + TRAILER_SIZE) {
        trig_length = (hdr.packet_length - HEADER_SIZE - TRAILER_SIZE) / 2;
    }

    if (trig_length == 0 || len < HEADER_SIZE + trig_length * 2 + TRAILER_SIZE)
        return result;

    double scale = hdr.adc_scale / POW_20;
    double range_factor = hdr.adc_range / FFFF;

    result.reserve(trig_length);
    for (size_t i = 0; i < trig_length; i++) {
        size_t offset = HEADER_SIZE + i * 2;
        uint16_t adc_raw = readU16BE(data + offset);
        double voltage = ((double)(adc_raw - hdr.adc_offset) * scale) * range_factor;
        result.push_back(voltage);
    }

    return result;
}

WaveformRaw extractWaveformRaw(const uint8_t* data, size_t len, const FrameHeader& hdr) {
    WaveformRaw result;
    size_t trig_length = 0;
    if (hdr.packet_length >= HEADER_SIZE + TRAILER_SIZE) {
        trig_length = (hdr.packet_length - HEADER_SIZE - TRAILER_SIZE) / 2;
    }
    if (trig_length == 0 || len < HEADER_SIZE + trig_length * 2 + TRAILER_SIZE) {
        result.offset = hdr.adc_offset;
        result.scale = hdr.adc_scale;
        result.range = hdr.adc_range;
        return result;
    }
    result.samples.reserve(trig_length);
    for (size_t i = 0; i < trig_length; i++) {
        size_t off = HEADER_SIZE + i * 2;
        result.samples.push_back(readU16BE(data + off));
    }
    result.offset = hdr.adc_offset;
    result.scale = hdr.adc_scale;
    result.range = hdr.adc_range;
    return result;
}
