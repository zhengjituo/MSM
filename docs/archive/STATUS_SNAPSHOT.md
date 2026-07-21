# MSM_DAQ 开发状态快照

## 时间
2026-07-08 17:30 CST，即将切换到远程 opencode 继续

## 当前运行状态
- `/home/zkcx/ws/MSM_DAQ/build/msm_daq_storage` 运行中（PID 3261817）
- 采集正常：4 通道，ch1 frame_size=1152，ch2-4 frame_size=128
- 已处理 ~114M 帧/通道，系统稳定（曾修复 displayFrames_ 竞态导致的 segfault）
- webui: `/tmp/live_server.py` 在 `:8088` 服务
- IPC socket: `/tmp/msm_daq.sock`

## 架构

### 数据流
```
PXIe1073+3010 → ModuleDAQ → ring buffer(256MB×4) → ModuleProcess(peek/parse) → SaveBatchAll MSG → ModuleStorage(fwrite to /mnt/localssd/daq_data/)
```

每 1 秒：`main loop` → `saveLiveJson()` → `buildStatusJson()` → `/tmp/msm_live.json` → `:8088/api/data`

### 控制流
`e2e_dashboard.html` → `POST /api/msg {body: "Start >> ->| Storage"}` → `live_server.py:send_ipc("MSG <body>\n")` → IPCServer Unix socket → MessageBus

### Web 端点
- `GET /api/data` → `/tmp/msm_live.json` 内容
- `GET /` → `e2e_dashboard.html`
- `POST /api/msg` → 转发 NI string 到 IPC socket

## 帧格式（frame_format.md 已更新）
帧头 64B + 波形数据 + 帧尾 64B。
- TrigID: header[17..20] (U32 BE)
- BunchID: trailer[2..7] (U48 BE)
- Sum: trailer[8..11] (U32 BE)
- X/Y/I: trailer[24..47] (DBL BE, FPGA 未填充)

## 模块当前状态

### ModuleProcess (`module_process/module_process.h/cpp`)
- ✅ `doConsumeOnce()`: peek 全批帧 → 提取 trig_id/bunch_id/sum → 存入 displayFrames_ (mutex 保护)
- ✅ `displayFrames_[ch]` 存储每通道最近 10000 帧元数据
- ✅ `getDisplayFrames(ch)` 返回副本 (const, lock-guarded)
- 🚧 `lastWaveform_`: 添加了成员和 getter，但编译未通过
- ❌ 上次 build 失败的编译错误（未查原因）

### ModuleDAQ (`module_daq/module_daq.h/cpp`)
- 原样运行，未修改

### ModuleStorage (`module_storage/module_storage.h/cpp`)
- 原样运行，未修改

### main_storage.cpp
- ✅ `buildStatusJson()` 含 trig_id/bunch_id/sum/x_pos/y_pos/intensity 数组
- 🚧 已添加 `ch["wf"]` = process.getLastWaveform() 的代码，未编译

### live_server.py (`/tmp/live_server.py`)
- ✅ POST /api/msg 转发到 IPC socket（需加 `\n` 后缀）
- ✅ GET /api/data 读取 /tmp/msm_live.json
- ✅ 单线程 HTTPServer

### e2e_dashboard.html (`module_daq/e2e_dashboard.html`)
- ✅ Start All / Stop All / Save ON / Save OFF 控制按钮
- ✅ TrigID 和 BunchID 图表（已移除 Sum/X/Y/I 图表）
- ✅ 逐帧检查器（trig_id / bunch_id / delta 校验）
- ❌ 缺少波形显示（用户需求）

## 未完成的功能

### 1. 波形显示 (P0)
- `lastWaveform_` 已添加到 ModuleProcess header/cpp
- `ch["wf"]` 已添加到 buildStatusJson
- **编译错误需修复**，然后添加 waveform chart 到 e2e_dashboard
- 波形从 each frame 的 offset 64 开始读取 `frame_size - 128` 字节，按 uint16 BE

### 2. 编译错误修复 (P0)
上次 build 失败，需要检查 cmake/make 输出定位原因。

## 已知问题
1. **IPCServer 单线程阻塞**：handleClient 在 recv() 上阻塞，导致只能同时处理一个 IPC 连接。live_server.py POST 需要确保连接快速完成。
2. **displayFrames_ 竞态已修复**：通过 `std::mutex displayMutex_`，lock_guard 保护 clear/push_back/read。
3. **ch1 与其他通道 frame_size 不同**：ch1=1152 (trig_length=512)，ch2-4=192 (trig_length=32)。
4. **X/Y/I 在 FPGA 中未填充**：读出来是 NaN/garbage。

## 关键文件位置
| 文件 | 远程路径 | 本地路径 |
|------|---------|---------|
| ModuleProcess.h | `module_process/module_process.h`, `module_daq/module_process.h` | `module_daq/module_process.h` |
| ModuleProcess.cpp | `module_process/module_process.cpp`, `module_daq/module_process.cpp` | `module_daq/module_process.cpp` |
| main_storage.cpp | `module_main/main_storage.cpp` | 不存在于本地 |
| live_server.py | `/tmp/live_server.py` | 不存在于本地 |
| e2e_dashboard.html | `module_daq/e2e_dashboard.html` | `module_daq/e2e_dashboard.html` |
| frame_format.md | `docs/frame_format.md` | `docs/frame_format.md` |
| msm_dashboard.html | `module_daq/msm_dashboard.html` | `module_daq/msm_dashboard.html` |

## 快速命令

```bash
# 构建
cd /home/zkcx/ws/MSM_DAQ
export PATH=$HOME/.local/lib/python3.10/site-packages/cmake/data/bin:$PATH
cmake -B build && make -j4 -C build msm_daq_storage

# 运行（screen 后台）
screen -dmS daq bash -c 'cd /home/zkcx/ws/MSM_DAQ && LD_LIBRARY_PATH=module_core/extern/lib:module_core/extern/epics/lib/linux-x86_64 EPICS_CAS_INTF_ADDR_LIST=127.0.0.1 EPICS_CA_SERVER_PORT=5064 EPICS_CA_ADDR_LIST=127.0.0.1 EPICS_CA_AUTO_ADDR_LIST=NO ./build/msm_daq_storage 2>&1 | tee /tmp/daq.log'
screen -dmS webui python3 /tmp/live_server.py

# 测试控制
curl -s -X POST -d "Start >> ->| Storage" http://localhost:8088/api/msg

# 直接 IPC（调试）
echo "MSG Status >> ->| Process" | nc -U /tmp/msm_daq.sock
```

## 下一步任务
1. 修复编译错误
2. 验证 lastWaveform_ + ch["wf"] 输出正确
3. 添加 waveform chart 到 e2e_dashboard
4. 添加 Sum 图表（可选）
5. 页面加 data 量显示

## 更新 1: 编译错误修复 (2026-07-08 17:45)
- **问题**: `ModuleProcess::getLastWaveform()` 在 `private:` 区段，
  `main_storage.cpp:73` 的 `buildStatusJson()` 无法访问。
- **修复**: 将 `getLastWaveform()` 声明移至 `private:` 之上（第 43 行），
  位于 `setStorageTarget()` 之后，`lastWaveform_` 成员保留在 private 区段。
- **状态**: ✅ `make -j4 msm_daq_storage` 编译通过，已生成可执行文件

## 待办
1. ⬜ 重启 `msm_daq_storage` 加载新的 waveform 输出
2. ⬜ 验证 `/tmp/msm_live.json` 中包含 `ch[].wf[]` 数组
3. ⬜ 在 `e2e_dashboard.html` 添加 Waveform 图表 (`#chartWf`)
