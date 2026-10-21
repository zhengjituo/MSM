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
- ring 大小取 `app_config.RingSizeMB`（默认 256MB，范围 8–1024）
- 卡集合变更（增/减卡）→ Init 明确报错「启用通道数变化」，**需重启进程**：
  ring/Process/Storage 通道数在进程启动时固定（main 按初始配置创建），
  硬件层面虽能 close+reopen，但下游管线不支持运行中扩容
- 通道 `TrigSource` 配置值（`global` 等名字）在 module_daq 内映射为 SDK 数值
  （SDK 只认数字，传名字会被 StringToUint32 拒绝）。多卡同步的前提：
  每张卡的通道都设为 `global`(4)，3010 的触发经背板广播到所有 1073
- 首次 SDK 初始化时先对机箱内**全部卡** open → `ResetCard` → close（毫秒级），
  清掉上次进程异常退出残留的触发/DMA 脏状态（否则可能只收到一个接收缓冲
  的量甚至 0 帧）
- 同步时序：Start 时先 `StartChannelRecv` 全部通道 → 500ms 稳定 → 3010
  TRIG=ON（板卡先配置启动，3010 触发物理滞后），保证首帧对齐

## 状态帧说明

| 帧 | 功能 |
|----|------|
| Init | 编排入口：顺序 ReadConfig → InitHW → UpdateConfig（hw 已就绪时跳过重开，仅推参） |
| ReadConfig | JSON → DAQ_Config 内存容器，分配 ring buffer |
| InitHW | SDK open+init 板卡（首次）；hw 已就绪时单独调用先 close 再重新 open+init |
| UpdateConfig | 单点写全部硬件寄存器（板卡级 + 通道级） |
| Start | 启动接收 → 2s settle → 3010 TRIG=ON |
| Stop | 3010 TRIG=OFF → flush → 停止通道 |
| CloseHW | close 板卡 → SDK 反初始化 |
| ReadOnce | 非阻塞读帧 → ring buffer |
