#include "frame_parser.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <vector>

static void printHex(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++)
        printf("%02x ", data[i]);
    printf("\n");
}

static bool readFile(const char* path, std::vector<uint8_t>& buf) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        std::cerr << "Error: cannot open " << path << "\n";
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        std::cerr << "Error: empty file\n";
        fclose(f);
        return false;
    }
    buf.resize((size_t)sz);
    size_t nread = fread(buf.data(), 1, (size_t)sz, f);
    fclose(f);
    if ((long)nread != sz) {
        std::cerr << "Error: short read\n";
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: test_parse <file.dat> [start_frame [frame_count]]\n";
        return 1;
    }

    const char* path = argv[1];
    size_t start = 0;
    size_t count = 0; // 0 = all

    if (argc >= 3) start = (size_t)atol(argv[2]);
    if (argc >= 4) count = (size_t)atol(argv[3]);

    std::vector<uint8_t> fileBuf;
    if (!readFile(path, fileBuf)) return 1;

    std::cout << "File: " << path << "\n";
    std::cout << "Size: " << fileBuf.size() << " bytes\n";

    // Determine frame size from first frame
    if (fileBuf.size() < MIN_FRAME_SIZE) {
        std::cerr << "File too small\n";
        return 1;
    }

    FrameHeader firstHdr;
    if (!parseHeader(fileBuf.data(), fileBuf.size(), firstHdr)) {
        std::cerr << "First frame header invalid\n";
        return 1;
    }

    size_t frameSize = firstHdr.packet_length;
    std::cout << "Frame size (from header): " << frameSize << " bytes\n";
    if (frameSize < MIN_FRAME_SIZE) {
        std::cerr << "Invalid frame size\n";
        return 1;
    }

    size_t totalFrames = fileBuf.size() / frameSize;
    std::cout << "Total frames in file: " << totalFrames << "\n";

    size_t trig_length = (frameSize - HEADER_SIZE - TRAILER_SIZE) / 2;
    std::cout << "Trig length: " << trig_length << " samples\n";
    std::cout << "WaveDataEnable: " << (trig_length > 0 ? 1 : 0) << "\n";

    if (count == 0 || start + count > totalFrames)
        count = totalFrames - start;

    std::cout << "Parsing frames [" << start << ", " << start + count - 1 << "]\n";
    std::cout << std::string(80, '=') << "\n";

    for (size_t f = start; f < start + count; f++) {
        const uint8_t* frame = fileBuf.data() + f * frameSize;

        FrameHeader hdr;
        if (!parseHeader(frame, frameSize, hdr)) {
            std::cout << "Frame " << f << ": header parse failed\n";
            continue;
        }

        const uint8_t* trailerData = frame + frameSize - TRAILER_SIZE;
        FrameTrailer tlr;
        if (!parseTrailer(trailerData, TRAILER_SIZE, tlr)) {
            std::cout << "Frame " << f << ": trailer parse failed\n";
            continue;
        }

        CalibratedFrame cal = calibrate(hdr, tlr);

        std::cout << "Frame " << f << ":\n";
        std::cout << "  Header:\n";
        std::cout << "    magic=0x" << std::hex << hdr.magic << std::dec
                  << "  pkt_len=" << hdr.packet_length
                  << "  type=" << (int)hdr.packet_type
                  << "  board=0x" << std::hex << hdr.board_type << std::dec
                  << "  ch=" << hdr.channel_num
                  << "  slot=" << (int)hdr.slot << "\n";
        std::cout << "    trig_id=" << hdr.trig_id
                  << "  ts_sec=" << hdr.timestamp_sec
                  << "  ts_ns=" << hdr.timestamp_ns
                  << "  fine_ts=" << (int)hdr.fine_ts << "\n";
        std::cout << "    adc_scale=" << hdr.adc_scale
                  << "  adc_offset=" << hdr.adc_offset
                  << "  adc_range=" << hdr.adc_range << "\n";

        std::cout << "  Trailer (raw):\n";
        std::cout << "    magic=0x" << std::hex << tlr.magic << std::dec
                  << "  bunch_id=" << tlr.bunch_id
                  << "  min=" << tlr.min_raw
                  << "  max=" << tlr.max_raw
                  << "  sum_l=" << tlr.sum_l
                  << "  sum_h=" << tlr.sum_h
                  << "  vpp=" << tlr.vpp_raw << "\n";
        std::cout << "    x_raw=" << tlr.x_raw
                  << "  y_raw=" << tlr.y_raw
                  << "  i_raw=" << tlr.i_raw << "\n";

        std::cout << "  Calibrated:\n";
        std::cout << std::fixed << std::setprecision(6);
        std::cout << "    bunch_id=" << cal.bunch_id
                  << "  min=" << cal.min_val
                  << "  max=" << cal.max_val
                  << "  vpp=" << cal.vpp << "\n";
        std::cout << "    sum_l=" << cal.sum_l
                  << "  sum_h=" << cal.sum_h
                  << "  x=" << cal.x
                  << "  y=" << cal.y
                  << "  i=" << cal.i << "\n";

        // Waveform: print first 10 samples
        if (trig_length > 0) {
            std::vector<double> wave = extractWaveform(frame, frameSize, hdr);
            std::cout << "  Waveform: " << wave.size() << " samples";
            if (!wave.empty()) {
                std::cout << "  first 10: ";
                size_t n = wave.size() < 10 ? wave.size() : 10;
                for (size_t k = 0; k < n; k++)
                    std::cout << std::setprecision(4) << wave[k] << " ";
            }
            std::cout << "\n";
        }

        std::cout << std::string(80, '-') << "\n";
    }

    std::cout << "Done. " << count << " frames parsed.\n";
    return 0;
}
