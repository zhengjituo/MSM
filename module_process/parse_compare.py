#!/usr/bin/env python3
"""Standalone frame parser CLI - extracted from ZkcxAnalysis_20260105.py for comparison."""
import sys, os, struct

POW_20 = 1 << 20
FFFF = 0xFFFF
EIGEN_DATA_TAIL_SIZE = 64
VOLTAGE_DATA_START = 64
MIN_PACKET_HEADER_LEN = 6
DEFAULT_FIXED_PACKET_LEN = 1024

FEATURE_BYTE_MAP = {
    "bunchId": (6, 2, "uint64"),
    "minValue": (2, 8, "int16"),
    "maxValue": (2, 10, "int16"),
    "lowHresholdCumulativeValue": (4, 12, "int32"),
    "highHresholdCumulativeValue": (4, 16, "int32"),
    "vppValue": (2, 20, "uint16"),
    # 22-23: reserved (2B)
    "offsetXValue": (8, 24, "int64"),    # ref: offset 24, not 22
    "offsetYValue": (8, 32, "int64"),    # ref: offset 32, not 30
    "lightIntensityValue": (8, 40, "int64")  # ref: offset 40, not 38
}

def GetUInt16(data): return struct.unpack(">H", data)[0] if len(data) >= 2 else 0
def GetInt16(data):  return struct.unpack(">h", data)[0] if len(data) >= 2 else 0
def GetUInt32(data): return struct.unpack(">I", data)[0] if len(data) >= 4 else 0
def GetInt32(data):  return struct.unpack(">i", data)[0] if len(data) >= 4 else 0
def GetUInt64(data):
    if len(data) == 0: return 0
    pad = 8 - len(data)
    if pad > 0: data = b"\x00" * pad + data
    elif pad < 0: data = data[-8:]
    try: return struct.unpack(">Q", data)[0]
    except: return 0
def GetInt64(data):  return struct.unpack(">q", data)[0] if len(data) >= 8 else 0

def ExtractCalibrationParams(data):
    PreGain = Offset = Range = 0
    if len(data) >= 41:
        PreGain = GetUInt32(data[37:41])
    if len(data) >= 45:
        Offset = GetUInt16(data[43:45])  # ref: 2B @ 43-44, not 4B @ 41-44
    if len(data) >= 49:
        Range = GetUInt16(data[47:49])
    return PreGain, Offset, Range

def ExtractRawVoltageData(packet_bytes):
    voltage_data = []
    if len(packet_bytes) < VOLTAGE_DATA_START + 2:
        return voltage_data
    PreGain, Offset, Range = ExtractCalibrationParams(packet_bytes)
    if PreGain == 0 or Range == 0:
        return voltage_data
    for i in range(VOLTAGE_DATA_START, len(packet_bytes) - EIGEN_DATA_TAIL_SIZE, 2):
        raw = packet_bytes[i:i+2]
        if len(raw) != 2: continue
        decode = GetUInt16(raw)
        voltage = ((decode - Offset) * PreGain / POW_20) * (Range / FFFF)
        voltage_data.append(voltage)
    return voltage_data

def ExtractFeatureFast(packet_bytes, feature_name):
    if feature_name not in FEATURE_BYTE_MAP:
        return 0.0
    data_len = len(packet_bytes)
    if data_len < EIGEN_DATA_TAIL_SIZE:
        return 0.0
    eigen_start = data_len - EIGEN_DATA_TAIL_SIZE
    eigen_raw = packet_bytes[eigen_start:eigen_start + EIGEN_DATA_TAIL_SIZE]
    if len(eigen_raw) != EIGEN_DATA_TAIL_SIZE:
        return 0.0
    field_len, offset, data_type = FEATURE_BYTE_MAP[feature_name]
    if offset + field_len > len(eigen_raw):
        return 0.0
    target = eigen_raw[offset:offset + field_len]
    if len(target) != field_len:
        return 0.0
    raw_value = 0
    if data_type == "uint16": raw_value = GetUInt16(target)
    elif data_type == "int16":  raw_value = GetInt16(target)
    elif data_type == "uint32": raw_value = GetUInt32(target)
    elif data_type == "int32":  raw_value = GetInt32(target)
    elif data_type == "uint64": raw_value = GetUInt64(target)
    elif data_type == "int64":  raw_value = GetInt64(target)
    if feature_name == "bunchId":
        return float(raw_value)
    PreGain, Offset, Range = ExtractCalibrationParams(packet_bytes)
    if PreGain == 0 or Range == 0:
        return float(raw_value)
    if feature_name in ["minValue", "maxValue", "vppValue"]:
        return float(raw_value * PreGain / POW_20 * (Range / FFFF))
    elif feature_name in ["offsetXValue", "offsetYValue"]:
        # ref: int64 / 2^32, NO PreGain/Range calibration
        return float(raw_value) / 4294967296.0
    elif feature_name == "lightIntensityValue":
        # ref: int64 / 2^32 * 2.0, THEN * PreGain / 2^20 * Range / 0xFFFF
        raw_div = float(raw_value) / 4294967296.0 * 2.0
        return raw_div * PreGain / POW_20 * (Range / FFFF)
    else:
        return float(raw_value * PreGain / POW_20) * (Range / FFFF)

FEATURE_NAMES = ["bunchId", "minValue", "maxValue", "lowHresholdCumulativeValue",
                 "highHresholdCumulativeValue", "vppValue", "offsetXValue",
                 "offsetYValue", "lightIntensityValue"]

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <file.dat> [start_frame [frame_count]]")
        sys.exit(1)
    path = sys.argv[1]
    start = int(sys.argv[2]) if len(sys.argv) >= 3 else 0
    count = int(sys.argv[3]) if len(sys.argv) >= 4 else 0

    if not os.path.exists(path):
        print(f"Error: {path} not found")
        sys.exit(1)
    file_len = os.path.getsize(path)
    print(f"File: {path}")
    print(f"Size: {file_len} bytes")

    with open(path, 'rb') as f:
        header = f.read(MIN_PACKET_HEADER_LEN)
        if len(header) < MIN_PACKET_HEADER_LEN:
            print("File too small"); sys.exit(1)
        pkt_len = GetUInt32(header[2:6]) or DEFAULT_FIXED_PACKET_LEN
        f.seek(0)
        first_frame = f.read(pkt_len)
        PreGain, Offset, Range = ExtractCalibrationParams(first_frame)
        trig_id = GetUInt32(first_frame[17:21]) if len(first_frame) >= 21 else 0

    frame_size = pkt_len
    total_frames = file_len // frame_size
    trig_length = (frame_size - 128) // 2 if frame_size >= 128 else 0
    print(f"Frame size (from header): {frame_size} bytes")
    print(f"Total frames in file: {total_frames}")
    print(f"Trig length: {trig_length} samples")
    print(f"WaveDataEnable: {1 if trig_length > 0 else 0}")
    print(f"First trig_id={trig_id}  PreGain={PreGain}  Offset={Offset}  Range={Range}")

    if count == 0 or start + count > total_frames:
        count = total_frames - start
    print(f"Parsing frames [{start}, {start + count - 1}]")
    print("=" * 80)

    for f_idx in range(start, start + count):
        offset = f_idx * frame_size
        with open(path, 'rb') as f:
            f.seek(offset)
            frame_bytes = f.read(frame_size)
        if len(frame_bytes) < frame_size:
            print(f"Frame {f_idx}: short read"); continue
        if frame_bytes[0:2] != b'\xeb\x91':
            print(f"Frame {f_idx}: bad magic"); continue

        tid = GetUInt32(frame_bytes[17:21])
        ts_ns = 0
        if len(frame_bytes) >= 32:
            ts_ns = (frame_bytes[27] << 32) | (frame_bytes[28] << 24) | \
                    (frame_bytes[29] << 16) | (frame_bytes[30] << 8) | frame_bytes[31]

        trailer = frame_bytes[-EIGEN_DATA_TAIL_SIZE:]
        bunch_id = int(ExtractFeatureFast(frame_bytes, "bunchId"))
        min_v    = ExtractFeatureFast(frame_bytes, "minValue")
        max_v    = ExtractFeatureFast(frame_bytes, "maxValue")
        sum_l    = ExtractFeatureFast(frame_bytes, "lowHresholdCumulativeValue")
        sum_h    = ExtractFeatureFast(frame_bytes, "highHresholdCumulativeValue")
        vpp      = ExtractFeatureFast(frame_bytes, "vppValue")
        x        = ExtractFeatureFast(frame_bytes, "offsetXValue")
        y        = ExtractFeatureFast(frame_bytes, "offsetYValue")
        i        = ExtractFeatureFast(frame_bytes, "lightIntensityValue")

        # Raw values
        min_raw  = GetInt16(trailer[8:10])
        max_raw  = GetInt16(trailer[10:12])
        sum_l_raw = GetInt32(trailer[12:16])
        sum_h_raw = GetInt32(trailer[16:20])
        vpp_raw  = GetUInt16(trailer[20:22])
        x_raw    = GetInt64(trailer[24:32])
        y_raw    = GetInt64(trailer[32:40])
        i_raw    = GetInt64(trailer[40:48])

        print(f"Frame {f_idx}:")
        print(f"  Header: trig_id={tid}  ts_ns={ts_ns}")
        print(f"  Trailer (raw): bunch_id={bunch_id}  min={min_raw}  max={max_raw}  sum_l={sum_l_raw}  sum_h={sum_h_raw}  vpp={vpp_raw}")
        print(f"    x_raw={x_raw}  y_raw={y_raw}  i_raw={i_raw}")
        print(f"  Calibrated: bunch_id={bunch_id}.0  min={min_v:.6f}  max={max_v:.6f}  vpp={vpp:.6f}")
        print(f"    sum_l={sum_l:.6f}  sum_h={sum_h:.6f}  x={x:.6f}  y={y:.6f}  i={i:.6f}")

        if trig_length > 0:
            wave = ExtractRawVoltageData(frame_bytes)
            print(f"  Waveform: {len(wave)} samples", end="")
            if wave:
                n = min(10, len(wave))
                vals = "  ".join(f"{wave[k]:.6f}" for k in range(n))
                print(f"  first 10: {vals}")
            else:
                print()
        print("-" * 80)

    print(f"Done. {count} frames parsed.")

if __name__ == "__main__":
    main()
