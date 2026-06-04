# Acquisition 架构

## 设计目标

- 每通道独立 reader + ring + consumer 三线程，零帧丢失（已验证 0 坏帧）
- 全局配置 `DaqConfig` 从 `app_inittial_config.json` 解析，main 极简
- RAII 管理 SDK 生命周期和采集通道
- 消费者 10k 帧/批，可通过 `onBatch()` 子类化扩展（存文件、网络等）

## 架构总览

```
                        ┌─────────────────────────────────────┐
DaqConfig ──load──→     │             Acquisition             │
  (全局配置文件)        │  (每通道一个实例)                    │
                        │                                     │
┌──────────────┐        │  ┌──────────┐     ┌──────────┐      │
│ DeviceContext│─ctx──→ │  │ reader   │──→  │ ring buf │      │
│ (SDK RAII)   │        │  │ thread   │     │ (64 MB)  │      │
│              │        │  │          │     │          │      │
│   open       │        │  │ReadChnFrm│write│monotonic│      │
│   init       │        │  │┌memcpy───│──→  ││64-bit   │      │
│   close      │        │  │└───────  │     │└──┐      │      │
│   deinit     │        │  └──────────┘     │   │      │      │
└──────────────┘        │                   │   │read  │      │
                        │                   └───▼──────┘      │
                        │                   ┌──────────┐      │
                        │                   │consumer  │      │
                        │                   │thread    │      │
                        │                   │          │      │
                        │                   │ 10k帧/批  │      │
                        │                   │ onBatch()│      │
                        │                   └──────────┘      │
                        └─────────────────────────────────────┘
```

## 组件说明

### DaqConfig (`daq_config.h/cpp`)

- 从 `app_inittial_config.json` 解析全机箱配置
- `CardConfig[]` + `ChannelConfig[]` 层级结构
- 所有通道共享同一份配置实例
- API: `findCardByType("1073")`, `getChannel(card_idx, ch)`

### DeviceContext (`acquisition.h/cpp`)

- 构造：`CxDeviceSdkInit` → `GetChassis` → `GetCards` → `open`
- 析构：`close` → `SdkDeinit`
- 存储每张板卡的 device_id 和 handle
- API: `configureBoard(idx, clock, sync)`, `findCardByType("1073")`

### Acquisition (`acquisition.h/cpp`)

单通道采集实例。构造时需传入 `DeviceContext`、`DaqConfig`、板卡类型和通道号。

**工作流**：
| 方法 | 阶段 | 说明 |
|------|------|------|
| `configure()` | 非采集态 | 从 DaqConfig 读本通道参数 → SDK 设通道参数 → 创建 ring buffer |
| `start()` | 采集态 | `StartChannelRecv` → `run` → 启动 reader + consumer 线程 |
| `stop()` | 停止 | 停止线程 → `stop` → Join |

**reader 线程**：
- 10ms 间隔循环 `CxDeviceReadChannelFrames`
- `memcpy` 到本地 buffer → `ring_->writeFromDevice()`
- 首次写入时通过 `rp[0].BufferSize` 获取实际帧长（非硬编码384）
- 仅计数成功写入的帧

**consumer 线程**：
- 等帧长稳定后，以 10000 帧为一批从 ring 读取
- 调用 `onBatch(data, n_frames, frame_len)` 处理
- `onBatch()` 为虚函数，可被子类重写用于存文件

### UserCircularBuffer (`user_circular_buffer.h/cpp`)

SPSC 无锁字节环形缓冲区。

- **64 位单调递增计数器** `write_total_` / `read_total_`，永不回绕
- `mod capacity_` 仅用于计算缓冲区的物理存储位置
- 正确性保证：
  - `available = write_total_ - read_total_` 无条件正确
  - 溢出检测：`avail + size > capacity_` → reject（无符号安全）
- 写-释放 / 读-获取内存序保证线程间数据可见性

## 关键数据流

```
SDK 内部缓冲区 (ch×16 MB)
   ↓ CxDeviceReadChannelFrames (ch1..ch4 串行，每 10ms)
RawPack[].Buffer (连续 n 帧, 每帧 rp[0].BufferSize 字节)
   ↓ memcpy (reader 本地 vector)
   ↓ ring_->writeFromDevice()
Ring Buffer (64 MB / 通道)
   ↓ ring_->readData (10000 帧/批)
Consumer 本地 vector
   ↓ onBatch()
打印 / 存文件 / 网络
```

## 已知限制

- **SDK 跨通道串行化**：多通道时单通道吞吐 ≈ 单通道的 1/N
- **Ring buffer 丢帧**：64 MB 在 ~200k fps 时约 48% 因 overflow 丢弃（需增大 buffer 或加速 consumer）
- **帧长稳定**：SDK 返回 `rp[0].BufferSize` 第一次后即为固定值，consumer 等稳定后再读取

## 扩展方式

```cpp
// 继承 onBatch 实现文件保存
class FileWriter : public Acquisition {
    void onBatch(const uint8_t* data, size_t n, size_t flen) override {
        fwrite(data, n * flen, 1, fp_);
    }
    FILE* fp_;
};
```
