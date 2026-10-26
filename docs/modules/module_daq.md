# module_daq — 采集模块

## 功能简介
控制 1073 采集卡 + 3010 触发卡，实现多通道数据采集。含配置加载、硬件初始化、启停控制。

## 输入
- `Init` — 完整 key-value 配置（触发方式、通道参数等）
- `ReadConfig` / `Configure` — 重加载配置到内存
- `InitHW` — 单独硬件初始化
- `UpdateConfig` — 单独写寄存器参数
- `Start` — 启动采集
- `Stop` — 停止采集
- `CloseHW` — 关闭硬件
- `Reset` / `Status` — 控制查询

## 输出
- N 个 ring buffer（SPSC lock-free），N = 配置中启用通道数（4/8/12/16）
- `Response` 消息（Init/Start/Status 的同步回复）；Init 失败时回
  `status=error` + `error=<原因>`

## 多卡规则（严格 slot 匹配）

- 打开集合 = 配置 `Cards` 里的 1073 slot ∩ 机箱扫描到的 slot，不做顺序兜底
- 配置里有但机箱扫不到的卡 → Init 失败，日志列出「配置 vs 扫描」差异，
  进程保持存活、不采集
- `3010` 卡：配置里有就必须存在（同理报错）；配置没有则不要求
- 机箱有、配置没有的卡：不打开，仅 INFO 日志
- 卡集合变化（增/减/slot 改）→ close + reopen；集合不变 → 只热重配参数
- 通道编号 = `Card#N` 在 JSON 中的顺序 × 卡内通道顺序
- `Status` 额外回传 `scanned_1073_slots` / `scanned_3010_slot`（机箱实际扫描结果，
  WebUI 作为配置提示）
- `Status` 每通道回传 `chN_slot`（机箱卡槽，-1 未知），WebUI 据此按卡分组展示：
  Frame Summary 每卡一行表头、Feature Table 每卡一张小表（多卡横向铺开）、
  波形每卡一个分节（slot 签名变化时重建）；无 slot 信息（sim/旧后端）回退平铺
- WebUI Config 面板支持增删板卡：新增 1073（通道参数深拷贝自已有 1073，
  slot 可从机箱扫描结果选未配置空位或手动输入）、新增/删除 3010（最多一张）；
  slot 冲突、重复 3010、超 4 张 1073（16ch 上限）均明确报错。
  增删写入待使用配置，**下次 Run（后端重启）生效**（下游管线通道数进程启动时固定）
- ring 大小取 `app_config.RingSizeMB`（默认 256MB，范围 8–1024）
- 卡集合变更（增/减卡）→ Init 明确报错「启用通道数变化」，**需重启进程**：
  ring/Process/Storage 通道数在进程启动时固定（main 按初始配置创建），
  硬件层面虽能 close+reopen，但下游管线不支持运行中扩容
- 通道 `TrigSource` 配置值（`global` 等名字）在 module_daq 内映射为 SDK 数值
  （SDK 只认数字，传名字会被 StringToUint32 拒绝）。多卡同步的前提：
  每张卡的通道都设为 `global`(4)，3010 的触发经背板广播到所有 1073
- 触发相关寄存器全部对齐厂商 demo 的 global 触发用例（`TrigMode`/`TrigPolarity`
  名字同样走数值映射，同款静默拒绝坑）：
  通道 `TrigMode` `normal`→`2`、`TrigPolarity` `Rising`→`1`/`Falling`→`0`；
  卡级 `GlobalTrigMode=0`（或逻辑）、`ExternalTrigLevel=20`
  - **双卡逐通道 bunch 相位已修复**：此前（触发寄存器未对齐 demo）双卡各通道相对
    ch1 有 10~80ms 固定相位（每 run 不同、零漂移）；对齐后 8 通道 288 批偏差 0µs。
    单卡一直正常（本地参考自洽）
  - 归因：最可能是 `TrigMode`——名字被 SDK 静默拒绝后各通道停在默认触发模式，
    默认模式在双卡下不与 3010 广播参考锁相。二分实验已排除卡级 `GlobalTrigMode`
    （去掉它相位仍为 0）；`TrigMode` 单独未做二分，但它是唯一能产生「逐通道」
    相位差且此前被静默拒绝的寄存器。若需 100% 锁定可再跑一轮去掉 `TrigMode` 映射
- 首次 SDK 初始化时先对机箱内**全部卡** open → `ResetCard` → close（毫秒级），
  清掉上次进程异常退出残留的触发/DMA 脏状态（否则可能只收到一个接收缓冲
  的量甚至 0 帧）
- 同步时序：Start 时先 `StartChannelRecv` 全部通道 → 500ms 稳定 → 3010
  TRIG=ON（板卡先配置启动，3010 触发物理滞后），保证首帧对齐

## 帧读回架构（每卡读线程）

- 读回由**每张 1073 卡一个专属读线程**驱动（`readLoop`），线程内轮询本卡通道；
  消息循环只处理控制消息（Init/Start/Stop/Status）
- 背景：8 通道单线程串行读时，每通道 SDK 调用 + memcpy ≈ 30ms → pass 周期 ≈ 175ms，
  超过 SDK 16MB 接收缓冲的保持时间（≈139ms@100kHz/1152B 帧）→ 缓冲 wrap 丢帧
  （双卡实测丢帧 ~8%、100ms 对齐 dist 退化至 15.7ms）。单卡 4ch 周期 ≈ 120ms 恰好不丢，
  所以单卡干净、双卡丢帧
- 每卡一线程后周期 ≈ 35~90ms（两卡并行）；SDK 缓冲加大到 **32MB**（≈280ms 保持）
  吸收读线程偶发停顿（实测 spike 至 ~195ms，多因 OS 调度/CPU 频率）
- SDK 返回的 n 帧**连续**（`RawPack[].Buffer` 指向同一连续区，厂商文档保证）：
  每通道首次读取验证连续性后走单次大 memcpy；不连续则回退逐帧拷贝（有日志）
- 环形缓冲（`UserCircularBuffer`）写路径非阻塞：满则整块丢弃并计 `ovf`，从不等待；
  实测真机 ovf=0
- 读线程在 Process 启动**之后**拉起（ring 从干净状态开始）；Stop 时先
  `joinReadThreads()` 再 `stopChannels/flushAll`，避免与停止/排空并发调同一卡句柄
- 诊断探针（常开，低频）：
  - pass 间隔 >50ms → `[ModuleDAQ] readpass gap: …ms (cardN slot=S, …)`
  - Stop 汇总 → `[DAQ] read-time: chN{api=…ms,max=…us,copy=…ms,max=…us}`
  - Stop 排空 → `[flush] chN api=…us n=…`

## 状态帧说明

| 帧 | 功能 |
|----|------|
| Init | 编排入口：顺序 ReadConfig → InitHW → UpdateConfig（hw 已就绪时跳过重开，仅推参） |
| ReadConfig | JSON → DAQ_Config 内存容器，分配 ring buffer |
| InitHW | SDK open+init 板卡（首次）；hw 已就绪时单独调用先 close 再重新 open+init |
| UpdateConfig | 单点写全部硬件寄存器（板卡级 + 通道级） |
| Start | 启动接收 → 500ms settle → 3010 TRIG=ON → 启动每卡读线程 |
| Stop | join 读线程 → 3010 TRIG=OFF → flush → 停止通道 |
| CloseHW | close 板卡 → SDK 反初始化 |
