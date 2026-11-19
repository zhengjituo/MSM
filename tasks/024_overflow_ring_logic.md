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

## 1024 点真机测试（hw_1024_sel.log，35s，8ch，512MB ring）

**结论：消费端已不是瓶颈；瓶颈上移到"卡→SDK 交付"段，且 60% 的丢失发生在
我们 ring 之前、当前完全不可见。**

数据（生产 = 100k 帧/s/通道 = 217.6MB/s）：
- process：read p50=4.3ms、early p50=15（基本无等待）→ 消费端快 ✓
- 但批率仅 3.5 批/s（被最慢卡供水限速），捕获 1.45M(card1)/1.23M(card2) 帧
- **前 62 批 100% 连续**（bunch 恰 +100000/批；109/123 批 dist=0）→ 链路本身
  无损能力没问题
- 第 63 批起（~5.5s，ring 渐满）批间出现 bunch 断点，ch1 ring ovf=209 次
  ≈1.31M 帧（37%）；card2 ovf=0
- **上游丢失**（card/SDK 内，无统计）：card1 余 ~0.7M（20%）；card2 ≈2.3M
  （65%！card2 读线程 gap 仅 3 次、交付却只有 35k/s → 丢失全在 ring 之前）
- card1 读线程 pass 周期 p50=230ms（78 次 gap>50ms）、SDK api p95=37ms
  → 4 通道串行轮询周期 > SDK 缓冲灌满时间（1024 点 ~73ms，512 点 ~139ms
  所以 512 干净）

**下一步候选**：
1. SDK 回调模式（`CxDeviceSetChannelRecvCallback` + `CxDeviceStartChannelRecv`，
   回调 `void (void* package, uint32_t length, void* userData)`）——数据到了
   即推，消除 230ms 轮询周期；需验证回调线程语义/每通道并发注册
2. 用 `CxDeviceGetChannelStatus` 探测卡/SDK 侧缓冲与溢出计数器——上游丢失
   必须可见（否则"零丢帧"无法证明）
3. 兜底：用户约束（降触发率至 50kHz / 点数上限 512）

## 第 2 项落地：上游（卡/SDK）丢失可见性

**SDK 探测结论**（sdk_probe，只读）：
- `CxDeviceGetChannelStatus`/`GetCardStatus` 候选名字（Overflow/Lost/Drop/Buffer
  等 40+ 个）全部 rc=-4 无效 → **SDK 不暴露任何丢帧计数器**
- `CxDeviceGetChannelFramesCount` 可用（通道号 1 起）→ SDK 缓冲占用可查
  （空闲实测残留 3854 帧/通道/卡，与 pass 大小吻合）
- `CxDeviceSetChannelRecvBufferSize` 存在（缓冲可配，后续调大候选）
- 回调模式 `CxDeviceSetChannelRecvCallback` 存在（第 1 项候选）

**实现（SDK 无计数器 → 软件推断）**：
- DAQ 读线程 per-frame bunch 连续性检查：1 bunch/µs 全局时钟，名义步长取
  最小间隙（100.828kHz → 9/10 交替，抖动不误报），gap > 1.5×步长 计缺失帧
  `(gap-step)/step` 四舍五入；跨 pass 连续（lastBunch 持久）；bunch 回绕/复位
  重新基线。per-pass 开销 ~0.2ms（1.5 万帧 trailer 扫描），可忽略。
  → `chN_sdk_drops`（上游丢失帧数）+ 节流告警 `UPSTREAM DROP: bunch gap N
  (step S), 缺 X 帧（累计 Y，卡/SDK 侧）`
- ring 丢帧精确化：`chN_ring_drop_frames`（= 丢弃字节/帧长，读循环+flush 两处）
- SDK 缓冲占用：`chN_sdk_buf_frames`（onStatus 时 GetChannelFramesCount，
  上游丢帧的前置指标）
- 暴露链：Status → pv_map slow 组 +24 PV（.db 142 条）→ webui 计数表
  "丢帧"列改帧数 + 新"上游丢"列（colspan 7）→ PyDM 帧数表 +上游丢列
  （.ui 重生成，表头 CH|DAQ|Process|Batches|Consumed|丢帧|上游丢）
- onReset 一并复位新计数器；构建 rc=0，.ui offscreen 加载 OK

## 1024 丢帧根因定案 + 方案 A 部署（2026-11-18，预热双 start 长窗口法）

### 误诊纠正（推翻"60% 上游丢失"结论）

- **"提前停止"之谜**：进程首个 start 要 **16-20s 做 3 卡 initHW**（open→reset→
  close 全周期），吃掉测试窗口——之前 35s 窗口实际触发只有 8-15s。铁证：日志
  文件创建时间 12:58:49.77，trigger off 12:59:20.809 = 脚本 stop（32ms 内）。
- **3.2e14 天文 sdk_drops** = kill -9 残留脏状态（SDK 缓冲旧数据），干净启动
  不出现（本轮 0 复现）。
- **512 不丢工况全窗口确认**（预热+40s，hw_512_long.log）：8 通道全部
  3,883,386 帧完全一致、ovf=0、批间 100% 连续。

### 1024 点真实丢失（50s 持续，hw_1024_long.log，双 start 第二轮 48.7s 窗口）

- card1：3.827M/4.874M ≈ **丢 21.5%**，第二轮 ring ovf 545 次（第一轮 0）
- card2：3.61M/4.874M ≈ **丢 26%**，ovf=0 → 纯卡/SDK 侧
- 丢失层两轮之间"换卡" = **零裕量争用**：单卡读线程 4 通道串行，通道访问周期
  ≈154ms ≈ 32MB SDK 缓冲灌满时间 154.2ms，CPU 抢占/睿频波动直接变丢帧
- 机器：i7-8850H 6 核 12 线程，热路径 = 2 读线程（32MB×2 memcpy/pass）+
  process（21.76MB/批）+ softIoc
- **异常 A**：24 次 `first_bunch_id=0`，全在卡2 = 批头帧 magic 坏（帧完整性）
- **异常 B**：861 条 HUGE GAP（gap≈1.71e14，48 位窄带聚集值，magic 还在）：
  卡1 从 pass 119（约 18s）起**每 pass 必现**——系统性单向 onset，嫌疑
  SDK 交接边界（每 pass 末帧 torn）或热状态切换；未定案

### 检测器两处 bug 修复

1. 名义步长 **min → 众数**（gapHist 64 bins，每 1024 gaps 重算 argmax）：
   启动触发毛刺（实测 gap=6）把 min 钉死 → 512 干净工况 151 条误报；修后 0
2. **onStop 清零 lastBunch**：停止间隙（秒级 bunch gap）被误计"上游丢"
   （每轮边界 4.3s×8ch）；修后 0

### 部署：方案 A + 点数限制（用户拍板，总带宽 ~800MB/s 含网络传输）

- **A**：SDK 缓冲 `CxDeviceSetChannelRecvBufferSize` 32MB → **128MB**
  （614ms@1024pt / 1.17s@512pt），吸收瞬态停顿；rc≠0 打告警
- **点数限制** `checkTrigLengthCap`（`main_hwdaq.cpp`，g_startFn 拦截，
  CLI/IPC 全 start 路径生效，明确错误信息）：
  - 每卡启用 ≥3 通道 → ≤512 点（8ch 全开：8×1152B×100k ≈ 921.6MB/s 总）
  - 每卡启用 ≤2 通道 → ≤1024 点（4ch：4×2176B×100k ≈ 870.4MB/s 总）
  - 绝对上限 1024
  - 帧长公式验证：`(TrigLength/NS)×2+128` → 512pt=1152B，1024pt=2176B
- **验证**（hw_cap_test / hw_1024_4ch.log）：
  - 8ch@1024 start → 拒绝："卡 slot=3 启用 4 通道（≥3）时点数上限 512…" ✓
  - 4ch@1024（2+2，128MB 缓冲，39s 窗口）：**3,912,217 帧/通道，4 通道完全
    一致，ovf=0，UPSTREAM DROP=0，卡1+卡2 批间 100% 连续 → 零丢失** ✓
  - 8ch@512 复验（hw_512_8ch_final.log）：33.7s 窗口 3,381,736 帧/通道×8
    完全一致，ovf=0，批间 100% 连续 → 128MB 缓冲下仍零丢 ✓

### 注意（配置字段）

- pending/runtime JSON 每通道两个使能字段：**`runEnable`（bool，应用级，
  决定 ring/采集）** 与 `Enable`（string，卡参数字段）。改启用通道必须动
  `runEnable`，误改 `Enable` 会被 ConfigImpl 忽略（本轮踩过）。

## 关波形长度探索（用户指定轮，2026-11-18）

**背景**：用户问关原始波形传输（WaveDataEnable=0）后长度能加到多少——
理论每次触发仅 128B 包头/包尾。
**结论：关波形后长度不再是带宽问题，而是卡侧采集能力（触发周期）问题；
8ch@9216pt（~93% 占空）零丢。**

### 管线兼容性（零代码改动，SDK 驱动帧长）

- 帧 = 128B = HEADER_SIZE(64) + TRAILER_SIZE(64)，恰好包头+包尾
- flen 由 SDK 返回 `rp[0].BufferSize` 驱动（module_daq.cpp:598）→ ring
  `setExpectedPacketSize` → process 按 ring 实时包长解析，自动适配 128B
- parseHeader/parseTrailer 各需 ≥64B ✓；extractWaveformRaw trig_length=0
  返回空 ✓；bunch_id 在 trailer 内 → 丢帧检测链（帧数/ovf/批间/
  UPSTREAM DROP）全部照常有效

### 点数限制豁免（checkTrigLengthCap 增补）

- 关波形带宽 = 128B×触发率，与点数无关（8ch 仅 ~102MB/s）→ 关波形通道
  不计入点数上限；全卡关波形则无点数上限（点数仅受卡侧采集限制）
- 开波形规则不变：≥3ch→512、≤2ch→1024、绝对 1024

### 实测（8ch 全通道关波形，预热双 start，~33-39s 窗口）

| TrigLength | 帧数/通道 | ovf | UPSTREAM DROP | 批间连续性 | 结论 |
|---|---|---|---|---|---|
| 4096 | 3,381,685（×8 一致） | 0 | 0 | 100%（仅轮边界） | 零丢 |
| 8192 | 3,381,709（×8 一致） | 0 | 0 | 100% | 零丢 |
| 9216 | 3,381,776（×8 一致） | 0 | 0 | 100% | 零丢 |

- 帧数与开波形 512pt 工况（3,381,7xx）差 <0.001% → 同窗口、零丢失
- 9216pt = 9.216µs 采集 / 9.918µs 触发周期 ≈ **93% 占空**
- **物理上限**：100,828Hz → 周期 9.918µs @1ns/pt → **~9918pt**（采集
  填满触发周期即无法再长）；9216→9918 剩 7% 未探，未再压（任务收敛）

### 任务收敛（用户拍板 2026-11-19）

- **限定工况下当前架构 + i7-8850H 性能够用**：限定工况 = 点数限制
  （含关波形豁免），由 checkTrigLengthCap 在 start 强制，违规拒绝启动
- 已验证零丢工况：8ch@512（开波形）、4ch@1024（开波形，2+2）、
  8ch@4096/8192/9216（关波形）

## 未做（后续，均不阻塞收敛）

1. **异常 B 根因**：garbage 帧（1.7e14，卡1 pass 119 起每 pass 必现，仅
   1024pt×每卡 4ch 重负载工况）——新点数限制已规避该工况；若要再跑该
   工况，先加诊断记录 garbage 帧在 pass 内位置（若恒为末帧 → SDK 交接
   边界问题，找 SDK 方确认）。
2. **异常 A**：卡2 批头帧 magic 坏（24 次，同工况）——与异常 B 疑同源。
3. **回调模式（方案 C）** 仍作后备：A+点数限制后各限定工况已零丢，
   无再上必要，除非未来放开工况。
4. **根因 B**：start 重载检测到 TrigLength 变化时清 ring 残留（跨长度
   热切换的场景才触发；常规流程每次 start 前已 stop 排空）。
5. 精确 100,000 Hz（现 100,828）等遗留低优先项不变。
6. 关波形如需 >9216pt（至 9918）：7% 裕量未测，先确认 SDK 对非 2 幂
   长度（如 9900）的接受度再压。
