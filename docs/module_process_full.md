# module_process_full — 处理模块（全帧提取版）

## 功能简介
从 ring buffer 批量 peek 全批帧，逐帧提取 TrigID/BunchID/Sum/X/Y/I，存为 BatchData。提供**跨通道对齐检查**：若 ch1~N 的首帧 TrigID 不一致，丢弃该 batch。

## 输入
- `ConsumeOnce` — 消费一个 batch
- `Configure` — BatchSize、StorageTarget
- `Start` / `Stop`
- `Status`

## 输出
- `SaveBatchAll` → Storage 模块（含全帧结构体数组）
- 跨通道对齐日志（t0/t1/t2/b0/b1/cnt）
- 可通过 getter 查询通道帧数、batch 数、最后 TrigID/BunchID

## 状态帧说明

| 帧 | 功能 |
|----|------|
| Start | 清零计数器，开始消费 |
| Stop | 停止，打印各通道帧数 |
| ConsumeOnce | peek 全批 → 逐帧提取 → 跨通道对齐检查 → Send SaveBatchAll |

## BatchData 结构

```cpp
struct BatchData {
    int batchNo;
    int count;                      // 有效帧数
    std::vector<uint32_t> trig_id;  // 每帧 TrigID
    std::vector<uint64_t> bunch_id; // 每帧 BunchID
    std::vector<uint32_t> sum;
    std::vector<double> x, y, i;
};
```
