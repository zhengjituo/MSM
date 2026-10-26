# module_storage — 存盘模块

## 功能简介
从 ring buffer 读取数据，fwrite 写入文件。支持自动 rotate（按文件大小），每通道独立文件。

## 输入
- `SaveBatchAll` — 批量存盘请求（含 nCh、batchSize）
- `Start` / `Stop`
- `Record` — 开始/停止录盘
- `Status`

## 输出
- 磁盘文件：`<basePath>/PD_s1_ch<N>_<seq>.dat`

## 保存开关与 Process 联动

- `EnableSave` / `DisableSave`（CLI `enablesave`/`disablesave`、IPC 同名命令）同时发给
  **Storage 和 Process**，语义一致：设置待生效标志，**下次 Start 生效**
- Start 编排（main `g_startFn`）按 `app_config.startStorage` 向两模块同步开关，
  再依次 Start Storage → Process → DAQ
- Process 仅在其 `saveEnabled_` 为真时才把批次写入 saveRing；关闭时 saveRing 保持空，
  Storage 无数据可读（不产生「写 ring → 丢弃」的无效拷贝）
- 关闭保存时的实测收益（双卡 8ch）：Process 57%→27% CPU，Storage 28%→~0%，
  后端合计 2.2 核 → 1.5 核
- 不采用「Storage 直接消费 DAQ ring」方案：DAQ ring 为 SPSC（单生产者单消费者），
  加第二个消费者需多消费者 ring 重设计，且会把磁盘背压直接耦合进采集环
  （磁盘慢 → DAQ ring 被慢消费者拖住 → ovf 丢帧），隔离层（saveRing）是刻意的

## 状态帧说明

| 帧 | 功能 |
|----|------|
| Start | 创建文件，开始录盘 |
| Stop | 关闭文件，打印通道帧数 |
| EnableSave / DisableSave | 设置录盘开关（与 Process 同步，下次 Start 生效） |
