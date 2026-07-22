# V3 WebUI Dashboard — 设计文档

## 动机

将 V1 CLI 交互（start/stop/enablesave/disablesave/帧数据打印）搬到浏览器，改造 module_main_v3 为可用的 WebUI。

## 整体架构

```
Web Browser                    HTTP Server (httplib)                  MessageBus / Modules
     │                              │                                      │
     │  POST /api/msg ─────────────→│ (命令入口: start/stop/enablesave/    │
     │    "start"                   │  disablesave/SetFrameIndex/reload/   │
     │    "enablesave"              │  quit/Configure 等 6 类)             │
     │                              │  ├─ "start"/"stop"/"reload"/"quit"  │
     │                              │  │   → server 自己处理 chain        │
     │                              │  ├─ "enablesave"/"disablesave"      │
     │                              │  │   → MessageBus → Storage         │
     │                              │  └─ "SetFrameIndex"                  │
     │                              │      → MessageBus → Process         │
     │                              │                                      │
     │                              │ Process 满 10k 帧                    │
     │                              │  ←─ BatchReady 消息 ─────────────────│
     │                              │  (来自 Process::doConsumeOnce)       │
     │                              │                                      │
     │  SSE /api/events ←───────────│                                      │
     │  所有数据一次推送             │                                      │
     │                              │                                      │
     │  GET /api/status ───────────→│ (可选拉取，页面加载/手动刷新)         │
```

核心思想：
- **命令单向**：WebUI 按钮 → POST /api/msg → HTTP server 处理或转发 MessageBus
- **数据单向**：Process 满 batch → BatchReady 消息 → HTTP server → SSE push → WebUI
- **HTTP server 取代 V1 CLI 线程**，注册为 Module("WebUI") 接收 BatchReady/Reload/Quit 消息

## API 端点

| Method | Path | Content-Type | 说明 |
|--------|------|--------------|------|
| `POST` | `/api/msg` | `text/plain` | 命令入口，body 为 NI 字符串 |
| `GET` | `/api/events` | `text/event-stream` | SSE 推送 |
| `GET` | `/api/status` | `application/json` | 可选拉取（页面加载/手动刷新） |
| `GET` | `/` | `text/html` | serve web/msm_daq_hw.html |

### POST /api/msg — 命令路由

server 收到 body 后按命令前缀分类：

| 命令 | server 行为 |
|------|------------|
| `start` | 执行 chain: `send("Start >> -@ Storage") → sleep(100ms) → send("Start >> -@ Process") → sleep(100ms) → send("Start >> -@ DAQModule")` |
| `stop` | 执行 chain: `send("Stop >> -@ DAQModule") → sleep(100ms) → send("Stop >> -@ Process") → sleep(100ms) → send("Stop >> -@ Storage")` |
| `reload` | 执行 reload 序列: stop chain → loadConfig → sendDaqInit → distributeConfig |
| `quit` | 设置 g_stop = true，触发 shutdown |
| `enablesave` | 转发 MessageBus: `EnableSave >> -@ Storage` |
| `disablesave` | 转发 MessageBus: `DisableSave >> -@ Storage` |
| `SetFrameIndex >> index=N ...` | 转发 MessageBus → Process 更新 frameIndex_ |
| `Configure ...` | 转发 MessageBus → 目标模块 |
| 其他 | 原样转发 MessageBus |

### GET /api/events — SSE 推送格式

`event: table_update`

```json
{
  "batch_no": 123,
  "running": true,
  "save_enabled": false,
  "frame_index": 42,
  "t": 1712345678,
  "channels": [
    {
      "ch": 1,
      "daq_frames": 1234567,
      "processed": 1230000,
      "batches": 123,
      "saved": 1200000,
      "trig_id": 99999,
      "bunch_id": 111111
    }
  ]
}
```

每次 BatchReady 消息触发一次推送，一次性包含所有通道的计数 + 所选帧的 trig_id/bunch_id。

## Module HTTP Server ("WebUI")

在 `main_hwdaq.cpp` 中，HTTP server 注册为 MessageBus 模块：

- `handleState("BatchReady")` — 读取 daq/process/storage 数据 → 写入所有 SSE 客户端
- `handleState("Reload")` — 设置 g_reload = true
- `handleState("Quit")` — 设置 g_stop = true

SSE 客户端管理：`std::vector<httplib::DataSink*> sseClients_`，加锁线程安全。

## Process 模块改动

### 新增

```cpp
// module_process.h
std::atomic<int> frameIndex_{0};  // 默认 0 (首帧)
void setFrameIndex(int idx);
int  getFrameIndex() const;
BatchData getLastBatchData(int ch) const;
```

- `handleState("SetFrameIndex")` → 解析 `index=N` → `frameIndex_ = N`
- `doConsumeOnce()` 满 batch 后 → 构造 BatchReady 消息 `send("BatchReady >> batchNo=N -@ WebUI")`
- `getLastBatchData()` 返回最近一次 batch 的首帧数据（已实现）

## Storage 模块改动

无新增接口。`getSavedFrames(ch)` / `getSavedBytes(ch)` 已被 SSE handler 使用。

## Module DAQ 改动

无新增接口。`getStats()` 和 `getRing(i)->getWriteTotal()` 已被 SSE handler 使用。

## WebUI (msm_daq_hw.html) 布局

```
┌──────────────────────────────────────────────────────┐
│ MSM DAQ  [● DAQ] [● Process] [● Storage]  1000 fps   │
├──────────────────────────────────────────────────────┤
│                                                        │
│  Save to disk ☐    ▶ Start    ■ Stop    ↻ Reload       │
│                                                        │
│  Frame index: [42]                                      │
│                                                        │
├──────────────────────────────────────────────────────┤
│  Ch │ DAQ帧    │ Process  │ Batches │ Saved    │ TrigID│ BunchID│
│ ────┼──────────┼──────────┼─────────┼──────────┼───────┼────────│
│  1  │ 1,234,567│ 1,230,000│ 123     │ 1,200,000│ 99999 │ 111111 │
│  2  │ 1,234,567│ 1,230,000│ 123     │ 1,200,000│ 99998 │ 111112 │
│  3  │ 1,234,567│ 1,230,000│ 123     │ 1,200,000│ 99997 │ 111113 │
│  4  │ 1,234,567│ 1,230,000│ 123     │ 1,200,000│ 99996 │ 111114 │
├──────────────────────────────────────────────────────┤
│  状态: 采集中  |  batch #123  |  14:30:22              │
└──────────────────────────────────────────────────────┘
```

### 交互

| 控件 | 行为 |
|------|------|
| Save ☐ | onChange 发 `enablesave`/`disablesave`，运行中 disable |
| ▶ Start | 发 `start` |
| ■ Stop | 发 `stop` |
| ↻ Reload | 发 `reload` |
| Frame index input | onChange 发 `SetFrameIndex >> index=N -@ Process` |
| EventSource | 连接 `/api/events`，收到 `table_update` 即渲染表格 |

### 状态灯

| 灯 | 条件 |
|----|------|
| DAQ ● | `running == true` |
| Process ● | 任一通道 `processed > 0` |
| Storage ● | 任一通道 `saved > 0` |

## 文件清单 (本任务涉及)

| 文件 | 操作 |
|------|------|
| `module_main_v3/main_hwdaq.cpp` | 重写，复用 V1 模块布线 + httplib HTTP server + SSE |
| `module_main_v3/web/msm_daq_hw.html` | 重写为新布局 |
| `module_main_v3/web/status.html` | 删（被 dashboard 取代） |
| `module_main_v3/CMakeLists.txt` | 确认 httplib 依赖（已有） |
| `module_process/module_process.h` | 新增 `frameIndex_` + `setFrameIndex` |
| `module_process/module_process.cpp` | `handleState` 处理 `SetFrameIndex` + `doConsumeOnce` 发 `BatchReady` |
