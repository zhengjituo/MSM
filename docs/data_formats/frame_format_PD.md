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
| 8–11 | 4B | **Sum**（U32，大端） | 光强积分值（各通道独立） |
| 12–23 | 12B | 保留 | |
| 24–31 | 8B | **X**（DBL，IEEE 754 大端） | 通道 1 仅有的 X 位置（设计中，当前 FPGA 未填充）|
| 32–39 | 8B | **Y**（DBL，IEEE 754 大端） | 通道 1 仅有的 Y 位置（设计中，当前 FPGA 未填充）|
| 40–47 | 8B | **I**（DBL，IEEE 754 大端） | 通道 1 仅有的强度（设计中，当前 FPGA 未填充）|
| 48–63 | 16B | 保留 | |

典型帧大小：
- `trig_length=512` → 帧头 64 + 1024 波形 + 帧尾 64 = **1152 字节**
- `trig_length=32`  → 帧头 64 + 64 波形 + 帧尾 64 = **192 字节**
- `EnableWaveForm=0` → 帧头 64 + 帧尾 64 = **128 字节**（无波形）

## JSON Live Bridge (`/tmp/msm_live.json` → `:8088/api/data`)

C++ 端每秒输出到 `/tmp/msm_live.json`，Python `live_server.py` 通过 `GET /api/data` 提供。

### 聚合统计字段

```json
{
  "running": true,
  "nch": 4,
  "channels": [
    {
      "ch": 1,
      "frames": 654282,
      "overflow": 0,
      "frame_size": 1152,
      "processed": 650000,
      "saved": 650000,
      "batches": 65,
      "trig_id":    [1, 2, 3, ...],         // U32
      "bunch_id":   [10000000000, ...],       // U64
      "sum":        [20, 25, 18, ...],        // U32
      "x_pos":      [0.5, 0.51, ...],         // DBL (ch1 only)
      "y_pos":      [0.5, 0.49, ...],         // DBL (ch1 only)
      "intensity":  [18.5, 18.7, ...]         // DBL (ch1 only)
    }
  ]
}
```

> **注意**: X/Y/I 字段当前 FPGA 固件尚未填充（预留后续升级），读取值为 NaN/0。

数组长度为 `min(batchSize, 10000)`，每 1 秒更新一次。

## 控制命令

| 按钮 | NI String |
|------|-----------|
| StartAll | `Start >> ->\| Storage` → `Start >> ->\| Process` → `Start >> ->\| DAQModule` |
| StopAll | `Stop >> ->\| DAQModule` → `Stop >> ->\| Process` → `Stop >> ->\| Storage` |
| Save ON | `SaveEnable >> ->\| Storage` |
| Save OFF | `SaveDisable >> ->\| Storage` |

通过 `POST /api/msg` 发送，Python 服务器转发到 IPC socket `<NIString>`。

## 帧尾解析代码示例

```cpp
// frame = raw frame buffer (frame_size bytes)
const uint8_t* trailer = frame + frame_size - 64;

// TrigID: header[17..20]
uint32_t trig_id = (frame[17] << 24) | (frame[18] << 16) |
                   (frame[19] << 8)  | frame[20];

// BunchID: trailer[2..7] (U48 big-endian)
uint64_t bunch_id = ((uint64_t)trailer[2] << 40) |
                    ((uint64_t)trailer[3] << 32) |
                    ((uint64_t)trailer[4] << 24) |
                    ((uint64_t)trailer[5] << 16) |
                    ((uint64_t)trailer[6] << 8)  | trailer[7];

// Sum: trailer[8..11] (U32 big-endian)
uint32_t sum = (trailer[8] << 24) | (trailer[9] << 16) |
               (trailer[10] << 8) | trailer[11];

// X/Y/I: trailer[24..47] (double big-endian, ch1 only)
// 当前 FPGA 未填充此段，保留字段。
double x = 0, y = 0, i = 0;
// 待 FPGA 固件升级后：读取 trailer+24 起 8 字节，大端转 double
```