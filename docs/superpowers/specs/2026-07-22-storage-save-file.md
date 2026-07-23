# Storage — 保存文件功能设计

## 数据流

```
DAQ ring[ch] ──→ ProcessFull (读帧/提取) ──→ saveRing (共享环形缓冲)
                                                  ↓
                                           Storage (写文件)
```

- ProcessFull 每通道每 batch（默认 10K 帧）写入 saveRing
- Storage 从 saveRing 读取，按通道分拆后 fwrite 到各通道独立文件
- Process 和 Storage 共享同一个 saveRing 指针，跨模块传递

## 生命周期

### 模块间的时序

**Start（由 main 发三条消息，严格按此顺序）**：
1. `Start >> -@ Storage` — 开文件，准备录盘
2. `Start >> -@ Process` — 开始消费 ring
3. `Start >> -@ DAQModule` — 开始采集/生成

**Stop（严格按此顺序）**：
1. `Stop >> -@ DAQModule` — 停采
2. `Stop >> -@ Process` — drain 残帧 → saveRing
3. `Stop >> -@ Storage` — drain saveRing 余量 → close 文件 → rename 加 BunchID

### Start
1. `onStart()` 设置 `recording_ = true`
2. 清空每通道的帧计数器、BunchID 记录（`firstBunchID`、`lastBunchID` 置零）
3. 创建临时文件（不含 BunchID）：`RAW-R{expNo}-...-Channel{ch}-S00000.tmp`
4. 所有通道同时 open（MSM 单状态机，步调一致）

### Running (onIdle)
1. 检查 saveRing 中是否有完整的 N 通道 × batchSize × frameSize 数据
2. 有则读取，按通道拆分
3. 若该通道 `firstBunchID` 为 0，从本 batch 第一帧解析 → `firstBunchID`
4. 每帧写入时记录 `lastBunchID`
5. fwrite 本 batch 数据
6. 写入后检查 `bytesWritten >= maxSaveSizeMB × 1MB`，如果满足则 rotate
7. Rotate：close 当前文件 → **rename 加上首尾 BunchID**（`.tmp` → `_B{first}_B{last}-S{seq}.dat`）→ 打开新临时文件（`{seq}+1`，此时无 BunchID，`firstBunchID` 重置待填充）
8. 所有通道同时 rotate（seq 值各通道对齐）

### Stop / Drain
1. `onStop()` 设置 `draining_ = true`
2. ProcessFull drain 阶段继续写入 saveRing
3. Storage drain：读光 saveRing 余量，每通道最后一帧也照写不误
4. saveRing 空后，满足切割条件也 rotate
5. close 所有文件，rename 加上首尾 BunchID
6. 打印每通道保存帧数/字节数
7. 设置 `recording_ = false`

## 文件命名

### 目录结构
```
{savePath}/{felName}/{systemName}/{deviceName}/{date}/{runSerial}/card{slot}/
```

| 部分 | 示例 | 说明 |
|------|------|------|
| `{savePath}` | `/mnt/localssd` | 根路径，可配置 |
| `{felName}` | `fel1` | 设施/线站名，配置 |
| `{systemName}` | `HSS` | 系统名，配置 |
| `{deviceName}` | `PD` | 设备名，配置 |
| `{date}` | `20260722` | YYYYMMDD，自动 |
| `{runSerial}` | `r00001` | 运行流水号（小写 `r`），来自 PV |
| `{slot}` | `3` | 板卡卡槽号，来自 DAQ config |

### 临时文件（写入中，不含 BunchID）
```
{savePath}/{felName}/{systemName}/{deviceName}/{date}/{runSerial}/card{slot}/RAW-R{expNo}-{systemName}-{deviceName}-Channel{ch}-S{seq}.tmp
```

### 关闭/rotate 时 rename（追加首尾 BunchID）
```
{savePath}/{felName}/{systemName}/{deviceName}/{date}/{runSerial}/card{slot}/RAW-R{expNo}-{systemName}-{deviceName}-Channel{ch}-B{firstBunchID}-B{lastBunchID}-S{seq}.dat
```

### 字段说明

| 字段 | 示例 | 说明 |
|------|------|------|
| `{expNo}` | `00001` | 实验号，来自 PV（与 `runSerial` 同一 PV，5 位补零，文件名中前缀 `R`） |
| `{ch}` | `1` | 物理通道号（非虚拟 0-based） |
| `{bunchID}` | `00000000000000` | 首帧 BunchID，48bit 值 14 位十进制补零 |
| `{firstBunchID}` | `00000000000000` | 该文件数据第一帧 BunchID |
| `{lastBunchID}` | `00000000012345` | 该文件数据最后一帧 BunchID |
| `{seq}` | `00000` | 自 start 后第几个切割文件，5 位补零（从 0 起） |

### 示例
```
/mnt/localssd/fel1/HSS/PD/20260722/r00001/card3/RAW-R00001-HSS-PD-Channel1-B00000000000000-S00000.tmp
  ↑ close/rotate →
/mnt/localssd/fel1/HSS/PD/20260722/r00001/card3/RAW-R00001-HSS-PD-Channel1-B00000000000000-B00000000012345-S00000.dat
```

## 切割规则

- 写入粒度 = Process batch（10K 帧）
- 切割只在完整 batch 写入后进行，不切在 batch 中间
- 触发条件：`bytesWritten >= maxSaveSizeMB × 1024 × 1024`
- 实际文件大小 = `batchSize × frameSize × N`（向上取整到 batch 倍数）
- 所有通道 seq 对齐：同一轮 onIdle 中同时 rotate，seq 值相同

## BunchID 解析

Storage 自行从原始帧数据中提取 BunchID，不依赖 Process：

```
帧结构 [Header 64B][波形数据 ...][Trailer 64B]
Trailer[+2..+7] = BunchID (48-bit big-endian)
```

- 首帧 BunchID：该文件写入的第一个完整 batch 的第一帧
- 尾帧 BunchID：该文件最后一个 batch 的最后一帧
- 单帧场景下首尾可相同
- 文件名中 BunchID 为 14 位十进制补零（48bit 最大 281474976710655，14 位够用）

## 物理通道号

- 来自 `DaqConfig::cards[].channels[].channel`
- 通过 `physChannels_` vector 传入 Storage 构造函数
- 在 `formatFilePath()` 中替换 `{ch}` 占位符
- V1 当前传 `{}` 回退到虚拟索引，启用 storage 时改为传真实物理通道号

## Status 反馈

`onStatus` 返回：
- `recording` — 是否录盘中
- `session` — 当前 session ID
- `ch{N}_saved_frames` — 每通道已保存帧数
- `ch{N}_saved_bytes` — 每通道已保存字节数
- `ch{N}_current_file` — 当前写入的文件名

## 配置参数

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| SavePath | string | `/mnt/localssd` | 根路径 |
| FelName | string | `fel1` | 线站名 |
| SystemName | string | `HSS` | 系统名 |
| DeviceName | string | `PD` | 设备名 |
| FileType | string | `dat` | 文件后缀 |
| MaxSaveSizeMB | int | 1024 | 单文件最大 MB |
| ExpNo | int | 1 | 实验号（来源 PV，5 位补零） |
| RunSerial | string | `r00001` | 运行流水号（来源 PV，小写 r） |
| BatchSize | int | 10000 | 写入粒度（帧数） |

## 与现有代码的兼容性

- `ModuleStorage` 类需重大修改：
  - 新增 BunchID 解析逻辑（首帧 + 尾帧）
  - 文件命名改用新目录结构（不再依赖 `formatFilePath` 模板，改为 `buildFilePath` 按字段拼接）
  - 新增配置字段：FelName、SystemName、DeviceName、ExpNo、RunSerial
  - 移除旧字段：RunId（由 ExpNo 替代）、FilePathTemplate（不再需要）
  - 配置改为从 `Configure` 消息读取新字段
- `saveRing_` 指针已存在于 ProcessFull 和 Storage，V1 当前传 `nullptr`，启用时传入真实 ring 指针即可
- V1 CLI 的 `storage.start()` 当前不调用，启用时在 main 中调用即可
- `ModuleProcess` 无需修改（已正确写入 saveRing）
