# 帧格式说明

## 物理帧结构

帧由 **64 字节帧头 + 波形数据 + 64 字节帧尾** 构成。

帧头（64 字节）:

| 偏移 | 大小 | 内容 |
|------|------|------|
| 0| 2B | 帧头魔术字 `0xEB 0x91` |
| 2 | 4B | 包长度（U32 大端） |
| 6 | 1B | 包类型标识 |
| 7 | 4B | 板卡类型（U32 大端）  |
| 11 | 2B | 采样率（U16 大端） |
| 13 | 1B | 分辨率（U8） |
| 14 | 2B | 卡内通道号（U16，大端） |
| 16 | 1B | 板卡slot号（U8） |
| 17 | 4B | **TrigID**（U32，大端） |
| 21 | 6B | 秒上时间戳计数s （48bit，大端）|
| 27 | 5B | 秒下时间戳计数ns （40bit，大端） |
| 32 | 1B | 补偿精细实践戳，和板卡有关，例如60ps |
| 33 | 4B | 外部采集开窗时间 |
| 37 | 4B | **ADC_SCALE**（U32，大端） |
| 41 | 4B | **ADC_OFFSET**（U32，大端） |
| 45 | 4B | **ADC_RANGE**（U32，大端） |
| 49 | 1B | ADC_PRECISION（U8），数据存储位宽（16/24/32位） |
| 50 | 1B | ADC_Decode_Mode（U8），数据存储解码方式（0：大端；1：小端） |
| 51 | 1B | POSITION，触发位置，表示次数据触发位置调整 |
| 52 | 1B | 固件版本号 |
| 53 | 10B | 保留 |
| 64 | nB | 波形数据,n= `trig_length × 2 字节（ADC 采样点单点占字节数），`EnableWaveForm` 控制是否包含。|

帧尾（64 字节）:

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0–1 | 2B | 尾魔术字 `0xEB 0x92` | |
| 2–7 | 6B | **BunchID**（U48，大端） | 采集时间戳，每帧 +10 |
| 8–9 | 2B | **Min**（U16，大端） | 最小值（各通道独立） |
| 10–11 | 2B | **Max**（U16，大端） | 最大值（各通道独立） |
| 12–15 | 4B | **Sum_L**（U32，大端） | 低于低阈值的采样点累加（各通道独立） |
| 16–19 | 4B | **Sum_H**（U32，大端） | 高于高阈值的采样点累加（各通道独立） |
| 20–21 | 2B | **Vpp**（U32，大端） | 高于高阈值的采样点累加（各通道独立） |
| 24–31 | 8B | **X**（DBL，IEEE 754 大端） | 仅通道 1 有的 X 位置|
| 32–39 | 8B | **Y**（DBL，IEEE 754 大端） | 仅通道 1 有的 Y 位置|
| 40–47 | 8B | **I**（DBL，IEEE 754 大端） | 仅通道 1 有的强度 |
| 48–63 | 16B | 保留 | |

典型帧大小：
- `trig_length=512` → 帧头 64 + 1024 波形 + 帧尾 64 = **1152 字节**
- `trig_length=32`  → 帧头 64 + 64 波形 + 帧尾 64 = **192 字节**
- `EnableWaveForm=0` → 帧头 64 + 帧尾 64 = **128 字节**（无波形）

## WebUI 状态推送

C++ webUI（`msm_webui`）通过 SSE 实时推送 DAQ/Process/Storage 状态及特征表数据：

```json
{
  "running": true,
  "save_enabled": true,
  "backend_alive": true,
  "backend_mode": "sim",
  "backend_status": "running",
  "batch_no": 42,
  "frame_index": 0,
  "channels": [
    {"ch":1, "daq_frames":654282, "proc_frames":650000, "proc_batches":65, "saved_frames":650000}
  ],
  "features": [
    {"ch":1, "trig_id":1, "bunch_id":10000000000, "min":0.0, "max":0.5, "sum_l":20.0, "sum_h":25.0, "vpp":0.5, "x":0.5, "y":0.49, "i":18.5}
  ]
}
```

## C++ 帧解析库

`module_process/frame_parser.h/cpp` 提供完整的帧解析实现，支持：

- `parseFrameHeader()` — 解析帧头（TrigID、ADC 参数）
- `parseFrameTrailer()` — 解析帧尾（BunchID、Min/Max/Sum_L/Sum_H/Vpp）
- `decodeX()`, `decodeY()`, `decodeI()` — 位置/强度解码（带 PreGain × Range 校准）

框架已通过 `test_parse` 与修正版 Python 解析结果 1:1 验证（296 笔数据集）。
