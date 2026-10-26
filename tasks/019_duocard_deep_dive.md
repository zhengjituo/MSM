# Task 019 — 双卡深挖：丢帧 / 处理效率 / bunch 相位 / 波形 tab

## 备份 git 号（恢复点）
```
defaafa  起始 commit（task 018 收尾后的 100ms 对齐提交）
91c412a  T1 fix(multi-card): 每卡独立读线程 + 32MB SDK 缓冲，消除双卡丢帧
a707931  T2 perf(process): 保存关闭时不写 saveRing，消除丢弃链路的无效拷贝
f6b8c07  T3 fix(multi-card): 触发寄存器对齐厂商 demo，消除双卡逐通道 bunch 相位
77aaf29  docs: 018 日志补记 + 不报厂商决策
6358e24  T4 feat(webui): 波形多卡按 tab 切换（卡内 2×2）
```
> 分支：`feat/multi-card`；2026-10-26 测试通过后 fast-forward 合入
> master（`e026a10..6358e24`，20 提交），tag **v1.1-multicard**。
> 回退：`git reset --hard defaafa`（合入前状态）。

## 背景
- task 018 双卡验证时发现：各通道 bunch_id 相对 ch1 有 10~80ms 固定相位
  （触发 ON 后 ~2s 内跳变到位、零漂移、每 run 数值不同）；单卡 4ch 恒为 0
- 用户判断：触发既然 global/normal、同源，不该飘；并怀疑 ring buffer /
  两张卡的 API 会被阻塞
- 后续用户追加两个问题：
  1. 8 通道 DAQ 有参差、Process 本该对齐，为什么处理效率还这么低；
     Process↔Storage 能否复用 DAQ 的 ring
  2. 波形多卡时应放 tab，一次只显示一张卡（卡内 2×2）

## TODO
- [x] T1 修双卡丢帧（~8%）
- [x] T2 修处理效率（存盘链路空转）+ 回答 ring 复用问题
- [x] T3 修双卡逐通道 bunch 相位（触发寄存器对齐 demo）
- [x] T4 WebUI 波形多卡 tab 切换
- [x] T5 文档、合入 master、打 tag

## 根因与修复

### T1 双卡丢帧 ~8%（100ms 对齐 dist 退化至 15.7ms）
- 现象定位：批间差 ≠100000 的批从第 132 批起密集出现；8 通道总缺失 17.3s/28s
- 读回计时探针（per-channel api/copy µs）+ pass 间隔探针（>50ms 打印）
- **根因**：8ch 单线程串行读回，每通道 SDK 调用+memcpy ~30ms → pass 周期
  ~175ms > SDK 16MB 接收缓冲保持时间 ~139ms（100kHz×1152B）→ 每 pass 缓冲
  wrap 覆盖 ~36ms 数据。单卡 4ch 周期 ~120ms < 139ms 恰好不丢
  （解释"单卡干净、双卡丢帧"）
- ring 本身无责：`UserCircularBuffer` 写满即丢+计数（不阻塞），全程 ovf=0
- 修复：
  - 每张 1073 卡一个专属读线程（`readLoop`），消息循环只处理控制消息
  - SDK 接收缓冲 16MB → 32MB（~280ms），吸收读线程偶发 OS 级停顿（spike ~195ms）
  - 利用厂商文档"RawPack[].Buffer 连续 n 帧"：首次验证后单次大 memcpy
- 验证：双卡 8ch×289 批零 gap、dist=0 全中；单卡回归 291/291

### T2 处理效率（后端 2.2 核 → 1.5 核）
- 每线程 CPU 实测：DAQ 读 72.8/66.0%，Process 57.3%，Storage 28.2%
- **根因**：`startStorage=false` 时存盘链路仍空转——Process 每批 92MB 写
  saveRing，Storage 排空后直接丢弃（920MB/s × 2 次拷贝纯浪费）
- 顺带发现：`app_config.startStorage` 解析了但**从未接线**（CLI/IPC
  enablesave 是唯一入口）
- 修复：
  - Process 增 `saveFlag_`/`saveEnabled_`（EnableSave/DisableSave 消息，
    语义与 Storage 一致：下次 Start 生效）；关闭时跳过 saveRing 写与空间检查
  - `g_startFn` 按 `startStorage` 向 Storage+Process 同步开关
  - CLI/IPC enablesave/disablesave 双发
- **ring 复用结论：不采用**——DAQ ring 是 SPSC，Storage 直读需多消费者
  ring 重设计，且磁盘背压会直接耦合进采集环（盘慢 → DAQ ring 被拖住 →
  ovf 丢帧）；saveRing 隔离层是刻意的
- 验证：Process 27.0%、Storage ~0%；关存路径 discard 归零、gap=0、dist 全中；
  开存路径（临时 startStorage=true）8ch×115MB/s 写 NVMe、512MB 轮转正常，
  改回 false 正确回切

### T3 双卡逐通道 bunch 相位（10~80ms → 0）
- **根因**：触发配置与厂商 demo（test_1073_global_trigger）不一致——
  通道 `TrigMode` 传字符串 `"normal"` 被 SDK 静默拒绝（StringToUint32，
  与 task 018 的 TrigSource 同坑），FPGA 停默认触发模式，双卡下与 3010
  广播参考不锁相 → 逐通道固定相位；`TrigPolarity` 传 `"Rising"` 同被拒；
  卡级 `GlobalTrigMode`/`ExternalTrigLevel` demo 有设、我们没设
- 修复：`mapTrigMode` normal→2、`mapTrigPolarity` Rising→1/Falling→0、
  卡级 GlobalTrigMode=0、ExternalTrigLevel=20
- 二分：去掉卡级 GlobalTrigMode 相位仍为 0 → 与/或逻辑非根因（保留为
  厂商参考值）；TrigMode 为最可能根因（唯一"逐通道"且被静默拒绝的寄存器，
  单独二分未做，如需 100% 锁定可再跑一轮）
- 验证：双卡 8ch×288 批相对 ch1 偏差 0µs、gap=0、dist=0 全中
- **用户判断证实**："触发设 global/normal 后本应同源不飘"——名字没映射成
  数字、等于没设

### T4 波形多卡 tab 切换
- ≥2 卡显示 tab 栏（Card#N · slot X），一次只显示一张卡；卡内 2×2
  （恢复单卡时代的排布，多卡分组那次曾变 1×4 窄条）
- 单卡/无 slot（sim/旧后端）无 tab；tab 栏 flex-wrap 自适应（4 卡=4 个 tab）
- 切 tab 对可见卡 canvas 重排版；卡集合变化重建 tab、活动 tab 越界回 0
- HTML 每次请求从磁盘读，浏览器刷新即生效，无需重编
- 验证：用户浏览器实测通过（2 卡 tab 切换正常）

## 决策
- **不报厂商**（用户拍板）：丢帧与相位均软件侧解决，无待确认硬件缺陷
- 不采用 Storage 直读 DAQ ring（理由见 T2）
- 低优先遗留：
  - 双卡每卡读线程 busy ~70-97%，当前 100.8kHz 下安全；若加长波形或提
    触发率，候选优化 = 省掉读路径中间 buffer 的一次 memcpy（需先确认 SDK
    读指针语义保证已消费区稳定）
  - 精确 100,000 Hz（现 100,828，3010 时钟偏差 0.8%）：未找到触发周期配置项

## 验证方式 / 真机数据（机箱 2×1073 slot3/5 + 3010 slot10）
- 编译：msm_cli_hwdaq / msm_cli_simsrc / msm_webui / msm_ipc_shell 全过
- 双卡 30s：8ch 批间差恒 100000（零丢帧）、dist=0 288/288、相位 0µs
- 单卡 30s 回归：291/291 dist=0、零 gap
- CPU（双卡、关存）：2.2 核 → 1.5 核（DAQ 64.7/58.9、Process 27.0、Storage ~0）
- 存盘（开存）：/mnt/localssd/fel1/HSS/PD/… 8ch×115MB/s、512MB 轮转
- webui：双卡 2 个 tab、2×2、切换正常（用户实测）

## 日志
- 2026-10-26 用户报"不飘这么多"（触发 global/normal 同源预期）→ 对照厂商
  demo 发现 4 项触发寄存器差异；用户提议单测 slot5 → 单卡 4ch 零漂移
  （矩阵：slot3 单卡 OK / slot5 单卡 OK / 双卡飘）
- 2026-10-26 用户问 ring/API 是否阻塞 → 加 pass 间隔 + api/copy 计时探针 →
  发现 pass 周期 175ms > 16MB 缓冲 139ms（T1 根因）；每卡读线程 + 32MB
  缓冲修复，双卡零丢帧（`91c412a`）
- 2026-10-26 用户问处理效率 + ring 复用 → 每线程 CPU 实测，定位存盘链路
  空转 + startStorage 未接线（T2），2.2→1.5 核（`a707931`）；ring 复用
  否决（SPSC/磁盘背压）
- 2026-10-26 触发寄存器对齐 demo → 双卡相位全零（T3）；二分排除
  GlobalTrigMode；用户问"为什么研究 and/or"→ 说明假设由来与排除结论
  （`f6b8c07`）
- 2026-10-26 用户拍板不报厂商；波形改 tab 切换（T4，`6358e24`）；
  用户测试通过 → fast-forward 合 master + tag v1.1-multicard
- 2026-10-26 **done**
