# module_process — 处理模块（轻量版，集成 frame_parser）

## 功能简介
从 ring buffer 批量 peek 帧，使用 `frame_parser` 库解析首帧全部特征字段（TrigID/BunchID/Min/Max/Sum_L/Sum_H/Vpp/X/Y/I），用于实时监控。支持自动 100ms 对齐和手动帧索引选择。

## 输入
- `ConsumeOnce` — 消费一个 batch
- `Configure` — BatchSize、StorageTarget、BunchIdStep
- `SetFrameIndex >> index=N` — 设置帧索引（-1 = 自动 100ms 对齐，0~9999 = 固定偏移）
- `Start` / `Stop`
- `Status`

## 输出
- `SaveBatchAll` → Storage 模块
- 首帧全部特征字段 → 内部 BatchFrameData
- onStatus 返回特征表数据（4 通道 × 10 特征）

## 自动对齐

`SetFrameIndex >> index=-1` 时自动计算 100ms 对齐偏移：
```
offset = (100000 - first_bunch_id % 100000) / bunchIdStep
```
`bunchIdStep` 通过 `BunchIdStep` 配置项设置，或从帧数据自动检测。

## onStatus 扩展字段

| 字段 | 来源 |
|------|------|
| `chN_trig_id` / `chN_bunch_id` | 首帧 |
| `chN_sum_h` | 最后 Sum_H |
| `chN_feat_*` | 当前 batch 首帧全部特征 |
| `x_pos` / `y_pos` / `intensity` | Ch1 解码值 |
| `frame_index` | 当前使用索引 |

## BatchFrameData 结构

```cpp
struct BatchFrameData {
    std::vector<uint32_t> trig_id;
    std::vector<uint64_t> bunch_id;
    std::vector<double> min_val, max_val;
    std::vector<double> sum_l, sum_h;
    std::vector<double> vpp;
    std::vector<double> x, y, i;
    int64_t batchNo = 0;
    size_t count = 0;
};
```
