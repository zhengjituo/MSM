# MSM_DAQ

MSM (Message State Machine) 架构的 PXIe 采集控制系统，管理 1073 采集卡 + 3010 触发卡。

## 命名约定

- 目录: `module_xxx/`
- 文件: `module_xxx.h` / `module_xxx.cpp`
- 类: `ModuleXxx`

## 构建和运行

```bash
cmake -S . -B build && cmake --build build
mkdir -p build/config && cp app_initial_config.json build/config/
sudo ./build/msm_daq_storage  # 硬件: 采集+存盘
sudo ./build/msm_daq_sim      # 模拟: 虚拟数据源
```

- C++14, pthread, vendored SDK: `sdk_include/{libCxDeviceCore.so,libxdma_api2.so}`
- **没有测试**、没有 CI、没有 formatter/linter 配置

## 架构

```
                                 ┌──────────────────┐
                                 │   ModuleDAQ      │
                                 │  loadConfig()    │
                                 │  onConfigure()   │
                                 │  onStart()       │
                                 │  onStop()        │
                                 │  doReadOnce()    │
                                 └────────┬─────────┘
                                          │ memcpy → ring buffer
                                          ▼
                              ┌───────────────────────┐
                              │  256MB SPSC ring × N  │
                              │  (64-bit monotonic    │
                              │   counter, no wrap)   │
                              └────────┬──────────────┘
                         ┌──────────────┴──────────────┐
                         │ peekData(1帧)               │ readData(batchSize)
                         ▼                             ▼
              ┌──────────────────┐         ┌──────────────────┐
              │  ModuleProcess   │  MSG    │  ModuleStorage   │
              │  doConsumeOnce() │ ──────► │  onSaveBatch()   │
              │  peek 第1帧      │         │  read + fwrite   │
              │  解析 TrigID/    │         │  状态机:          │
              │   BunchID       │         │   start/stop/     │
              │  打印 → 终端    │         │   rotate file     │
              └──────────────────┘         └──────────────────┘
                                                      │
                                                      ▼
                                              /mnt/localssd/
                                              PD_s1_ch1_0.dat
                                              PD_s1_ch1_1.dat  ...

  IPCServer:
    Unix Socket /tmp/msm_daq.sock → MSG → NIStringParser → MessageBus
```

### 数据流（msm_daq_storage）

```
ModuleDAQ → ring → ModuleProcess (peek 不移动指针, 解析第一帧)
                 → ModuleStorage (收到 SaveBatch MSG → readData 消费 → 写文件)
```

- **ModuleProcess** 只 peek 不消费：调用 `ring->peekData(buf, flen)` 读第 1 帧解析，**不移动读指针**
- **ModuleStorage** 真正消费：收到 `SaveBatch` MSG 后调用 `ring->readData()` 移动读指针
- 两份模块共享同一个 ring buffer 和同一个读指针，不拷贝数据

## 通信

### 命令行 / IPC

```
quit / exit              ← 退出
status                   ← 打印帧数统计
Configure >> <fields> -@ DAQModule   ← 覆盖采集参数
Configure >> BatchSize=N -@ Process  ← 覆盖批大小
Configure >> ... -@ Storage          ← 配置存储参数
Start >> -@ Storage                  ← 开文件
Start >> -@ Process                  ← 启处理
Start >> -@ DAQModule               ← 启采集
Stop >> -@ DAQModule                ← 停采集
Stop >> -@ Process                   ← 停处理
Stop >> -@ Storage                   ← 关文件
Reset >> -@ DAQModule               ← 重置统计
```

```bash
echo "MSG Status >> -@ DAQModule" | socat - UNIX-CONNECT:/tmp/msm_daq.sock
```

**NIStringParser 必须使用 `>> -@` 格式**，即使无参数：`"Start >> -@ DAQModule"` ✅ vs `"Start -@ DAQModule"` ❌ (target 解析为空，消息静默丢弃)

## JSON 配置

### DaqParameter (DAQ 硬件参数, 必需)

```json
{
    "DaqParameter": {
        "ChassisNo": 1,
        "Cards": {
            "Card#1": {
                "CardIndex": 1, "CardType": "1073",
                "Slot": 5,
                "Channels": {
                    "Channel#1": {
                        "runEnable": true,
                        "channel": 1,
                        "TrigSource": "global",
                        "TrigMode": "normal",
                        "TrigLength": "1024",
                        "TrigLevel": "50",
                        "TrigPulseWidth": "16",
                        "VoltageRange": "1000",
                        "TrigPolarity": "Rising"
                    },
                    "Channel#2": { ... }, "Channel#3": { ... }, "Channel#4": { ... }
                }
            },
            "Card#3010": {
                "CardIndex": 3010, "CardType": "3010",
                "Slot": 10
            }
        }
    },
    "app_config": {
        "isEpicsControlMode": true, "dataSavePath": "/mnt/localssd",
        "fileName": "PD", "fileType": "dat", "maxSaveSizeMB": 1024,
        "runId": 234, "startStorage": false, "autoAcqInEpicsWhenSoftwareStarts": false
    }
}
```

**帧长公式**: `TrigLength * 2 + 128` 字节

## 通道与板卡匹配

- **最大 4 卡 × 4 通道 = 16 通道** (`MAX_CH=16`)
- 扫描硬件收集所有 1073 句柄 → 按 **PXIe slot 升序排序** → 顺位匹配 JSON 中 Card#1/Card#2/...
- 配置多出硬件 → 跳过；硬件多出配置 → 不用
- `runEnable=true` → 采集该通道；`false` → 跳过

## SDK 关键顺序

```
CxDeviceSetSdkOption/CxDeviceSdkInit
  → CxDeviceGetChassis/CxDeviceGetCards
    → CxDeviceGetCardSlotNo/CxDeviceGetCardDeviceId
    → CxDeviceSetCard(h, "open")
    → CxDeviceSetCardValue(h, "ClockSource", "0")
    → CxDeviceSetCard(h, "init")
    → CxDeviceSetCardValue(h, "SyncWindowEnable", "0") / CxDeviceSetCardValue(h, "SyncSource", "0")
      → 每个通道: GlobalTrigEnable=1, TrigSource/TrigMode/TrigLength/TrigPulseWidth/VoltageRange/TrigLevel
    → CxDeviceSetChannelRecvBufferSize(h, ch, 16MB)
    → CxDeviceStartChannelRecv(h, ch) → CxDeviceSetChannel(h, ch, "run")
  → 读: CxDeviceReadChannelFrames(h, ch, &frames, &n)
  → 停: CxDeviceStopChannelRecv → CxDeviceSetChannel(..., "stop") → CxDeviceSetCard(h, "close")
  → CxDeviceSdkDeinit
```

### 3010 触发卡

```
init:   SyncTrigTrigMux=2, SyncTrigGlbSync=1, TrigSource=2, SoftwareSync=1
启动:   CxDeviceWriteReg(0x1C020, 1)
停止:   CxDeviceWriteReg(0x1C020, 0)
```

### 读数据注意

- `CxDeviceReadChannelFrames` 返回的 `RawPack[].Buffer` 指针在**下次调用同一通道前**有效
- **每帧必须立即 `memcpy`** 到本地缓冲区，再写入 ring buffer
- SDK 串行化跨通道读取：多通道时单通道吞吐 ≈ 单通道时的 1/N
- GlobalTrigEnable=1 + TrigSource=global → 走 3010 触发脉冲
- **绝不设空 callback** → 空回调导致 ReadChannelFrames TID 乱序

## 已知隐患

- Ring buffer 256MB 在高帧率时有 overflow 风险
- 退出时关触发后需 200ms + flush 所有通道，否则最后一个通道残余数据丢失
