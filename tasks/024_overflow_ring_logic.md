# 024 丢帧治理：overflow 暴露 + ring 可配 + 消费瓶颈计时

## 背景（承接 023 排查结论）

多卡 1024 点数下复现"抽取帧 bunch_id 尾巴非 00000 / process 乱"：
- **根因 A（丢帧失步）**：每通道 ring"满即整批丢弃"，1024 点数生产
  217.6MB/s > process 消费 ~155MB/s，256MB ring 约第 39 批灌满开始丢；
  丢帧每通道独立 → 各通道"帧号→bunch"映射永久漂移，全局 frame_index
  只能对齐 ch1。
- **根因 B（帧长切换损坏）**：stop 期间改 TrigLength 且 ring 有残留时，
  新帧长跨旧帧长切帧 + recalcAlignCapacity 改物理映射 → 个别通道字节级
  永久错乱（独立问题，未修，见"未做"）。

## 用户决策（方向纠正）

> 丢帧不可接受。要么优化逻辑、改大 ring 大小，要么降低触发频率、限制
> 采集点数（后两者靠约束用户行为）。先从逻辑优化 + ring 入手；
> **一旦 overflow，必须暴露出来。**

不做"每通道各自找 00000 自对齐"——那会掩盖丢帧本身。

## 本次实施

### 1. overflow 全链路暴露（之前 ring 丢帧完全静默）

| 层 | 改动 |
|---|---|
| 后端日志 | `module_daq.cpp` 读线程 `writeFromDevice` 返回 < bytes 时打 `chN OVERFLOW: ring 满，本 pass 丢 X 帧（累计 Y 次丢弃）`，节流每通道 1 条/秒（`ChSlot::lastOvfLogMs`）；`flushAll` 路径同样告警 |
| Status→PV | `chN_overflow` 原本就在 DAQ Status 里；`pv_map.json` slow 组 +8 条 `DAQ:chN_overflow`（90→98），桥按 pv_map 通用映射，零代码改动；`.db` 110→118 条记录（已就地重生成） |
| webui | `main_hwdaq.cpp` ChannelStatus +`overflow`（取 `chN_overflow`）+ JSON 输出；`msm_daq_hw.html` 计数表加"丢帧"列，>0 红色加粗，cardrow colspan 5→6 |
| PyDM | `gen_pydm.py` 帧数表加"丢帧"列（绑 `DAQ:chN_overflow`）；`.ui` 已重生成（8 个 overflow 控件，offscreen 加载验证 OK） |

### 2. ring 大小显式化 + 加倍

- `app_config.RingSizeMB`：加载（`config_impl.cpp` findInt，默认 256）与
  保存两侧代码原本就有，但配置文件里缺键。现在 pending/runtime 都显式
  **RingSizeMB=512**（每通道；8 通道共 4GB，机器 15GB/可用 11GB）。
- ring 在 **backend 进程启动时**按配置分配（`[main] rings: N x 512MB`
  启动日志），改值需重启 backend。
- 如实说明：ring 加大只延缓灌满、吸收突发；稳态丢帧率 = 生产−消费，
  要归零必须靠第 3 项让消费≥生产。

### 3. 消费瓶颈计时（为"逻辑优化"提供数据）

`module_process.cpp doConsumeOnce` 每批追加一行：

```
[Process] batch=N t_us read=A scan=B ext=C log=D early=E
```

- read = 8×ring readData + saveRing 写；scan = bunch step 检测 +
  00000 对齐扫描；ext = 8×extractFrame；log = 全部 iostream 输出
- **early** = 本批之前 `doConsumeOnce` 早退（等 8 通道齐 10000 帧）的
  轮询次数：大 → 被供数限速（突发式写入/最慢卡），~0 且单批耗时长 →
  CPU 瓶颈。据此决定优化方向（memcpy/日志削减 vs 分批策略）。

## 配置恢复

- 复现期把全 8 通道 TrigLength 改成 1024 → **已恢复 512**（pending+runtime）。
- 保留 `RingSizeMB=512`（本次目的之一）。

## 验证状态

- `cmake --build build -j 8` rc=0，无告警。
- `.db` 118 条（含 8×`DAQ:chN_overflow` ai）；`.ui` offscreen 加载 OK
  （帧数表头 CH|DAQ|Process|Batches|Consumed|丢帧）。
- 待用户侧：重启 backend + EPICS 栈后跑 1024 stop/start 复现场景——
  预期看到 丢帧 列递增、OVERFLOW 日志、以及新的 `t_us ... early=` 行
  （30s 采样后把日志给我，定优化方向）。

## Consumed 语义对齐（用户拍板 A）

**问题**：不存时 `Consumed` 恒 0，与 webui 老注释"不管是否落盘都计数"不符。
**根因**：storage 侧恒排空 ring2 并计数（discard 路径），但 process 侧自
`a707931`（10-26，省无效拷贝）起**仅在保存启用时喂 ring2** → 不存时 ring2
无水，计数 0。老注释是"恒写 era"（7238d1b 起）的语义残留。
**决策（A）**：保留 perf（不存不喂 ring2），语义改准——
Consumed = 实际过 storage 阶段的帧数，仅保存启用时计数；不存时 0 是
预期，process 阶段消费（ring1 恒被消费）看 Process 列。
**落地**：webui 注释 + `Consumed` 表头 tooltip、gen_pydm 同文案 tooltip
（.ui 重生成，offscreen 验证 2 个 tab 各 1 个 tooltip），零性能改动。
（B 方案"恢复恒喂"被否：不存时每批 +2×174MB memcpy ≈ 2.4GB/s@7批/s，
与零丢帧目标相冲。）

## 逻辑优化第一步：不存时选择读（用户约束"绝对不阻塞原始数据流"）

**背景**：用户评估 100kHz × 4ch@1024 点 = 870MB/s/卡 ≈ PCIe gen3 x4（~3.94GB/s）
的 22%，物理带宽不是瓶颈；瓶颈在软件搬运链。约束：**process 任何环节不得阻塞
原始数据流**（ring 写满即丢、无锁已保证；唯一间接威胁 = process CPU 抢占拖慢
2 个 DAQ 读线程 → SDK 缓冲保活超时）。

**微基准（i7-8850H，单线程，1024 点/8ch/10000 帧批）**：
read(174MB 整批 memcpy) ~26ms = 99.7%；scan(挑选帧,10000 trailer) 123µs；
extract(分析帧,8×1024 样本) 24µs；log 66µs → 挑选/分析帧本身已足够快，
大头是白拷贝（不存时 174MB 读进 batchBuf_ 后全部丢弃）。

**实施**：
- `UserCircularBuffer` 新增 `peekAt(off,dest,size)`（读游标+off 窥读，不动游标）
  + `skip(n)`（游标前移丢 n 字节），均无锁、仅消费端调用；
  单元验证：网格/wrap 双 memcpy/avail 守卫/并发写端全过。
- `doConsumeOnce` 保存关时改选择读：ch1 整批 readData（对齐扫描用，21.76MB）
  + 其余 7 通道 peekAt(selIdx 帧)取帧 + peekAt(0)首帧日志，游标统一 skip 前移
  （ch>1）；保存开路径不变（整批读 + saveRing 全量写）。
  纯消费端改动，写端零影响。

**验证（SIM 冒烟，8ch）**：63 批零错误（无 peekAt/skip/短读/overflow）；
8 通道每批 first_bunch_id 完全一致（1→100001→… 恰 +100000/批，无漂移）；
t_us read 从首批 9.6ms 收敛到 **~1.5-2ms**（原 ~26ms，13× 降幅）；
scan~300µs / ext~20µs / log~65µs 与微基准一致。

## 未做（后续）

1. **逻辑优化第二步（等 HW 数据）**：1024 场景 30s 采样 `t_us read/scan/ext/log
   + early`。若 early 大 → 供数节奏问题（SDK pass 粒度/周期）；若 read 仍偏大
   → 进一步（如 ch1 整批也改 trailer 稀疏读）；若 CPU 竞争仍高 → 削减每批
   iostream / 线程亲和。选择读（第一步）已落地，HW 验证后定下一步。
2. **根因 B**：start 重载检测到 TrigLength 变化时清 ring 残留。
3. **用户约束**：触发频率/点数上限的校验与提示。
4. 精确 100,000 Hz（现 100,828）等遗留低优先项不变。
