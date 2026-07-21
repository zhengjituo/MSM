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

## 状态帧说明

| 帧 | 功能 |
|----|------|
| Start | 创建文件，开始录盘 |
| Stop | 关闭文件，打印通道帧数 |
| Record | 切换录盘开关 |
| SaveBatchAll | 从 ring 读取 batchSize 帧 → fwrite |
