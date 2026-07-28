# module_process_full — 处理模块（全帧提取版）

## 功能简介
从 ring buffer 批量 peek 全批帧，逐帧提取全部特征字段，存为 BatchFrameData。提供**跨通道对齐检查**：若 ch1~N 的首帧 TrigID 不一致，丢弃该 batch。

## 输入
- `ConsumeOnce` — 消费一个 batch
- `Configure` — BatchSize、StorageTarget
- `Start` / `Stop`
- `Status`

## 输出
- `SaveBatchAll` → Storage 模块（含全帧结构体数组）
- 跨通道对齐日志（t0/t1/t2/b0/b1/cnt）

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
