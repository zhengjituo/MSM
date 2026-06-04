# 1073 采集卡正确函数调用流程

## 三大阶段总览

```
阶段1：配置（非采集态）
  ├── DeviceContext 构造 → SDK init, 打开所有板卡
  ├── configureBoard()  → ClockSource, init, SyncWindow
  └── Acquisition::configure() → 通道参数（GlobalTrig, TrigSource, ...）

阶段2：启动（全部通道统一）
  └── Acquisition::start() → StartChannelRecv + run + 读线程
      ↑ 此时 3010 可以开始触发输出

阶段3：停止（先停触发 → 再停采集）
  ├── Acquisition::stop() → 停止读线程 + stop 通道
  └── DeviceContext 析构 → close 板卡 + SdkDeinit
```

**重要原则：**
- 所有参数配置在非采集态完成，一旦 start 后不再改参数
- 多通道时先全部 configure，再全部 start
- 停止必须先停外部触发，再停采集，最后释放板卡

---

## 阶段1a ─ DeviceContext（SDK 初始化 + 板卡枚举）

```cpp
#include "acquisition.h"

DeviceContext ctx;  // 构造时：SdkInit → GetChassis → GetCards → open 所有板卡
```

- 全局只创建一次（RAII）
- 析构时自动 `close` 所有板卡 + `SdkDeinit`
- `ctx.cardIndex(0x1073)` 返回 1073 板卡的索引
- `ctx.cardHandle(idx)` 返回板卡句柄

## 阶段1b ─ 板卡级配置（非采集态）

```cpp
ctx.configureBoard(card_idx, clock_source, sync_window, sync_source);
```

内部调用（固定顺序）：
```cpp
CxDeviceSetCardValue(card, "ClockSource", clock_source);
CxDeviceSetCard(card, "init");           // 加载 FPGA 固件
CxDeviceSetCardValue(card, "SyncWindowEnable", sync_window);
CxDeviceSetCardValue(card, "SyncSource", sync_source);
```

## 阶段1c ─ 通道级配置（非采集态）

```cpp
Acquisition ch(ctx, card_idx, channel);
ch.configure("hardware_config.json");
```

`configure()` 内部调用：
```cpp
CxDeviceSetChannelValue(card, ch, "GlobalTrigEnable", "1");
CxDeviceSetChannelValue(card, ch, "TrigSource", "4");       // 4=global
CxDeviceSetChannelValue(card, ch, "TrigMode", "2");
CxDeviceSetChannelValue(card, ch, "TrigLength", "128");
CxDeviceSetChannelValue(card, ch, "TrigPulseWidth", "16");
CxDeviceSetChannelValue(card, ch, "VoltageRange", "1000");
CxDeviceSetChannelValue(card, ch, "TrigLevel", "200");
```

**关键：此处不调用 StartChannelRecv / run**（那是 start() 的事）。

## 阶段2 ─ 统一启动采集

```cpp
ch.start();
```

内部顺序：
```cpp
CxDeviceSetChannelRecvBufferSize(card, ch, 16MB);
CxDeviceStartChannelRecv(card, ch);    // SDK 内部开始接收
CxDeviceSetChannel(card, ch, "run");   // 通道进入运行态
// 启动独立读线程，循环调用 ReadChannelFrames
```

多通道时先全部 configure 再全部 start。

## 阶段3 ─ 停止采集

```cpp
ch.stop();
```

内部顺序：
```cpp
running_ = false;           // 通知读线程退出
reader_thread_.join();      // 等待线程结束
CxDeviceSetChannel(card, ch, "stop");  // 通道停止
```

**停止顺序必须**：先停 3010 触发输出 → 再 stop() 采集 → DeviceContext 析构释放板卡。

---

## 完整示例

```cpp
#include <thread>
#include <vector>
#include "acquisition.h"

static const size_t FRAME_LEN = 384;
static const size_t BATCH_SIZE = 10000 * FRAME_LEN;

static uint32_t getTrigId(const uint8_t* p) {
    return ((uint32_t)p[17] << 24) | ((uint32_t)p[18] << 16) |
           ((uint32_t)p[19] << 8) | (uint32_t)p[20];
}

int main() {
    // 阶段1：配置
    DeviceContext ctx;
    int card1073 = ctx.cardIndex(0x1073);
    ctx.configureBoard(card1073, "0", "0", "0");

    Acquisition ch(ctx, card1073, 1);
    ch.configure("hardware_config.json");

    // 阶段2：启动
    ch.start();

    // 消费（10k帧粒度）
    std::vector<uint8_t> batch(BATCH_SIZE);
    for (int i = 0; i < 10; i++) {
        while (ch.availableDataSize() < BATCH_SIZE)
            std::this_thread::yield();
        ch.readData(batch.data(), BATCH_SIZE);
        printf("batch %d: first TrigID=%u\n", i+1, getTrigId(batch.data()));
    }

    // 阶段3：停止（先停外部触发，再停采集）
    ch.stop();
    // DeviceContext 析构自动关板卡
    return 0;
}
```

## 多板卡 / 多通道示例

```cpp
DeviceContext ctx;

// 枚举所有 1073
for (int c = 0; c < ctx.cardCount(); c++) {
    uint32_t did;
    CxDeviceGetCardDeviceId(ctx.cardHandle(c), &did);
    if (did != 0x1073) continue;

    ctx.configureBoard(c, "0", "0", "0");

    // 启用哪些通道由配置决定（当前示例：通道1-4全开）
    std::vector<Acquisition*> chs;
    for (int ch = 1; ch <= 4; ch++) {
        auto* a = new Acquisition(ctx, c, ch);
        a->configure("hardware_config.json");
        chs.push_back(a);
    }

    // 全部启动
    for (auto* a : chs) a->start();

    // 消费...

    // 全部停止
    for (auto* a : chs) a->stop();
    for (auto* a : chs) delete a;
}
```

## 重要禁令

- ❌ **绝不设空 callback**：`CxDeviceSetChannelRecvCallback` 会使 `ReadChannelFrames` 返回乱序数据
- ❌ **不需要 ReadAndWait**：`ReadChannelFrames` 本身已 0 丢帧
- ✅ **必须立即 memcpy**：SDK 返回的 Buffer 指针在下次调用前有效
```
