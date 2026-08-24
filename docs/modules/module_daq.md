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
- 4 个 ring buffer（SPSC lock-free），每通道一个
- `Response` 消息（Start/Status 的同步回复）

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
