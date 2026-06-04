# 1073 数据包（帧）格式

## 帧大小计算

```
128 点 @ 1GS/s → 128 个采样点
每点 2 字节（int16）
有效波形数据 = 128 × 2 = 256 字节
对齐到 32 点 → ((128 + 31) / 32) × 32 = 160 点 → 320 字节
帧长 = 64(帧头) + 320(数据) + 64(帧尾) = 448 字节？ 实测值 = 384 字节
```

**实测帧长：384 字节**（由 `rp[0].BufferSize` 给出）

## 帧内布局

所有多字节字段均为**大端（Big-Endian）**字节序。

### 帧头（偏移 0-63）

| 偏移 | 长度 | 字段 | 类型 | 说明 |
|------|------|------|------|------|
| 0 | 2 | FrameMagic | uint16 BE | 固定值 `0xEB91`，帧起始标识 |
| 2 | 4 | FrameLength | uint32 BE | 整帧长度（不含？含？待验证） |
| 6 | 2 | DeviceID | uint16 BE | 设备型号 `0x1073` |
| 8 | 9 | Unknown | uint8[9] | 含义未知 |
| **17** | **4** | **TriggerID** | **uint32 BE** | **触发序号，0 起始，每触发+1** |
| 21 | ... | WaveformData | int16[] | 波形采样点，每点 2 字节大端 |
| 21+(N*2) | ... | (填充) | | 对齐到 32 点边界 |

### 帧尾（偏移 FrameLength-64 及之后）

| 偏移 | 长度 | 字段 | 类型 | 说明 |
|------|------|------|------|------|
| FrameLength-64 | 2 | TailMagic | uint16 BE | 固定值 `0xEB92`，帧结束标识 |
| FrameLength-62 | 6 | **BunchID** | uint48 BE | Bunch 序号 |
| FrameLength-56 | 8 | ? | | 含义未知 |

### 关键偏移说明

**TriggerID** 提取（已验证）：
```cpp
// 偏移 17 的 4 字节大端
uint32_t tid = ((uint32_t)p[17] << 24) | ((uint32_t)p[18] << 16) |
               ((uint32_t)p[19] << 8) | (uint32_t)p[20];
```

**BunchID** 提取（已验证）：
```cpp
// 包尾 -64 处是 EB92，之后 6 字节为 BunchID
const uint8_t* tail = p + frame_len - 64;  // tail[0..1]=EB92
uint64_t bid = ((uint64_t)tail[2] << 40) | ((uint64_t)tail[3] << 32) |
               ((uint64_t)tail[4] << 24) | ((uint64_t)tail[5] << 16) |
               ((uint64_t)tail[6] << 8) | (uint64_t)tail[7];
```

> **注意**：BunchID 是 SciAcqPackage 转发时的批号（256 帧一批），
> 不是每个包的独立标识。TriggerID 才是按帧递增的唯一序号。

## 帧连续性验证

```cpp
static uint32_t getTrigId(const uint8_t* p) {
    return ((uint32_t)p[17] << 24) | ((uint32_t)p[18] << 16) |
           ((uint32_t)p[19] << 8) | (uint32_t)p[20];
}

// 跨批次检查
uint32_t prev_last = 0;
int first = 1;
for each batch of frames:
    uint32_t first_tid = getTrigId(batch[0]);
    uint32_t last_tid  = getTrigId(batch[n-1]);
    if (!first) {
        int gap = (int)(first_tid - prev_last);
        if (gap != 1) total_gaps += (gap - 1);
    }
    first = 0;
    prev_last = last_tid;
```

## 已知但未解析的字段

| 偏移 | 长度 | 推测 |
|------|------|------|
| 8 | 9 | 可能是板卡时间戳或状态字 |
| FrameLength-56 | 8 | 未知（可能包含 Timetag、SampleCount 等） |

## 典型配置参数

| 参数 | 值 | 说明 |
|------|-----|------|
| SampleRate | 1 GS/s | 1073 最大采样率 |
| TrigLength | 128 ns | 触发后采集时窗 |
| 每帧采样点数 | 128 | `TrigLength × 1GS/s / 1000` |
| 每帧字节数 | 384 | 实测值（含包头尾） |
| 100kHz 时数据率 | ~36.6 MB/s | `384B × 100k` |

> 如需精确帧长计算：`64 + ((trig_length_ns × sample_rate_mhz / 1000 + 31) / 32) × 32 × 2 + 64`
> 但实测 384 字节对应 128 点配置；建议直接从 `rp[0].BufferSize` 获取。
