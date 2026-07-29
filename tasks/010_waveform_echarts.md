# Task 010 — 波形原始数据提取 + ECharts 前端波形显示

## 起始 commit
```
66aa6a5 waveform: add raw waveform data path (extractWaveformRaw + Base64 IPC + /api/waveform HTTP + canvas display)
```

## TODO
- [x] `frame_parser.h/cpp`: 新增 `WaveformRaw` 结构体 + `extractWaveformRaw()` 返回原始 ADC uint16_t + 校准参数
- [x] `ModuleProcess`: `BatchFrameData` 加 `wave_raw/offset/scale/range`；`extractFrame()` 调 `extractWaveformRaw()`；`onStatus()` Base64 序列化
- [x] `main_webui/main_hwdaq.cpp`: `FeatureRow` 加波形缓存；`parseSnapshot()` 解析；`GET /api/waveform` 返回所有通道波形 JSON
- [x] `web/msm_daq_hw.html`: 全局 Waveform 开关 + 2×2 ECharts 网格（深色主题、dataZoom 缩放、tooltip、坐标轴）
- [x] `TrigPosition` 默认值 178→0（匹配 Qt 参考）
- [x] `Configure` 路径加 `updateConfig()`，reload 正确写硬件
- [x] Reload 不再 auto-start，等待用户手动 Start
- [x] ECharts 窗口双向缩放适配（`min-width:0` + `requestAnimationFrame` + debounced resize）
- [x] 编译验证全部通过
- [x] 推送到 GitHub

## Session Git Log

### 2026-07-29  waveform raw data + ECharts display
```
66aa6a5 waveform: add raw waveform data path
ddca7c3 webui: replace raw Canvas with ECharts waveform charts
6c3a6f9 chore: gitignore update only (app_runtime_config)
b2fab0e fix: TrigPosition default 178->0
a02d466 fix: Configure handler also calls updateConfig()
025b86b fix: reload does not auto-start DAQ
9d96707 fix: echart chart div width 100% + defer init
2d0deec fix: replace window.resize with ResizeObserver
fc0ce8d fix: grid min-width:0 + debounced resize + rAF init
```

## 日志

### 设计决议
- 波形数据走 `SSE 通知 + HTTP 按需拉取`（`GET /api/waveform`），不塞进 SSE
- 原始 ADC int16 → Base64 LE 传输，JS 端做 `voltage = (raw - offset) * scale / 2^20 * range / 65535` 校准
- WebUI 全局 "Waveform" 开关控制是否拉取显示，不按通道独立
- ECharts 替代原生 Canvas，获得坐标轴、缩放、tooltip 等专业图表功能

### 已修正
- Reload 时 `Configure` 路径缺 `updateConfig()`，参数只写内存不写硬件
- Reload 自动 Start，改为人手点 Start
- ECharts 容器 `min-width:auto` 导致窗口缩小时不缩放

## 结束 commit
```
fc0ce8d fix: grid min-width:0 + debounced window resize instead of ResizeObserver + requestAnimationFrame for init
```
