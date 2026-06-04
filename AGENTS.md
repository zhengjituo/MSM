# MSM_DAQ

MSM (Message State Machine) 架构的 PXIe 采集控制系统，管理 1073 采集卡 + 3010 触发卡。

## 构建和运行

```bash
cmake -S . -B build && cmake --build build
mkdir -p build/config && cp app_inittial_config.json build/config/
sudo ./build/msm_daq          # 需要 sudo 访问 xdma 驱动
```

- C++14, pthread, vendored SDK: `sdk_include/{libCxDeviceCore.so,libxdma_api2.so}`
- **没有测试**、没有 CI、没有 formatter/linter 配置
- Config 路径在 main.cpp:44 硬编码为 `config/app_inittial_config.json`

## 架构

```
main:
  DAQModule (控制+数据)
    loadConfig(path)          ← 解析 JSON → DaqConfig 结构体（手写 parser, 非 nlohmann）
    onConfigure(msg)          ← MSG overlay 字段（TrigLength/VoltageRange/TrigLevel 等）
    onStart()                 ← SDK Init → 扫描硬件 → slot 排序 → 匹配配置 → 初始化 → reader 线程 → 3010 触发
    onStop()                  ← 关触发 → 200ms 等残余 → flush → 关 3010 → 停 reader → cleanup
    readerLoop()              ← 轮询所有活跃通道 ReadChannelFrames → memcpy → ring buffer
         │
         ▼  256MB SPSC ring buffer / 通道 (64-bit 单调计数器, 永不回绕)
         │
  ConsumerModule (消费)
    consumerLoop(chIdx, ring) ← 等 batchSize 帧 (默认 10000) → readData → onBatch()
    onBatch()                 ← 虚函数, 默认打印 TrigID/BunchID

  IPCServer (控制)
    Unix Socket /tmp/msm_daq.sock → MSG → NIStringParser → MessageBus
```

## 通道与板卡匹配

- **最大 4 卡 × 4 通道 = 16 通道** (`MAX_CH=16`)
- 扫描硬件收集所有 1073 句柄 → 按 **PXIe slot 升序排序** → 顺位匹配 JSON 中 Card#1/Card#2/...
- 配置多出硬件 → 跳过；硬件多出配置 → 不用
- `runEnable=true` → 采集该通道；`false` → 跳过

## 通信

### 命令行 / IPC

```
quit / exit              ← 退出
status                   ← 打印帧数统计
Configure >> <fields> -@ DAQModule   ← 覆盖采集参数
Configure >> BatchSize=N -@ Consumer ← 覆盖批大小
Start >> -@ DAQModule                ← 启动采集
Start >> -@ Consumer                 ← 启动消费者
Stop >> -@ DAQModule                 ← 停止采集
Stop >> -@ Consumer                  ← 停止消费者
Reset >> -@ DAQModule                ← 重置统计
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
            "Card#1": {                                   // 按扫描 slot 顺位匹配
                "CardIndex": 1, "CardType": "1073",
                "Slot": 5,                                // 仅日志
                "Channels": {
                    "Channel#1": {
                        "runEnable": true,
                        "channel": 1,
                        "TrigSource": "global",           // global→3010, localChannel→自触发
                        "TrigMode": "normal",             // normal 或 autoMode
                        "TrigLength": "1024",             // ns; 帧长 = TrigLength*2 + 128
                        "TrigLevel": "50",                // mV
                        "TrigPulseWidth": "16",           // ns
                        "VoltageRange": "1000",           // mV
                        "TrigPolarity": "Rising",
                        ...
                    },
                    "Channel#2": { ... }, "Channel#3": { ... }, "Channel#4": { ... }
                }
            },
            "Card#3010": {                                 // 触发卡 (Channels: null)
                "CardIndex": 3010, "CardType": "3010",
                "Slot": 10
            }
        }
    },
    "app_config": {                                         // 可被解析但当前 main.cpp 未使用
        "isEpicsControlMode": true, "dataSavePath": "/mnt/localssd",
        "fileName": "PD", "fileType": "dat", "maxSaveSizeMB": 1024,
        "runId": 234, "startStorage": false, "autoAcqInEpicsWhenSoftwareStarts": false
    }
}
```

**帧长公式**: `TrigLength * 2 + 128` 字节

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

- Ring buffer 256MB (硬编码于 `daq_module.cpp:93`) 在高帧率时有 overflow 风险
- 退出时关触发后需 200ms + flush 所有通道，否则最后一个通道残余数据丢失
- Config 路径硬编码在 `main.cpp:44`，可改为命令行参数
- `app_config` 段虽然被解析但未在 main.cpp 中使用
