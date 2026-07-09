# 20260618 MSM DAQ EPICS 集成 — 交接文档

## 完成情况

### 核心框架 (module_core)
- EPICS base 已定位并拷贝到 `module_core/extern/epics/`（含 include/lib/bin/dbd）
- CMake 选项 `MSM_ENABLE_EPICS=ON/OFF` 控制 PVServer 编译
- PVServer 已实现：softIOC fork → generateDB → CA 通道 → pushToIOC 双向同步
- RESTServer 已实现：`GET/PUT /api/:module/:var` + `POST /api/msg`（NI 字符串转 MessageBus）

### DAQ 模块 (module_daq)
- **ModuleSim**: `SimMem` 结构体（trig_length/running/ch1-4_frames），`getVarBase()` / `PV_UPDATE` 推送
- **ModuleProcess**: `ProcessMem` 结构体（ch1-4 trig_id/bunch_id/frames + running），每 batch 更新 PV
- **ModuleStorage**: `StorageMem` 结构体（recording/ch1-4_frames/bytes），存盘后更新 PV
- **main_sim.cpp**: 全模块注册 + PVServer/RESTServer 初始化 + CLI 循环

### 验证结果（20060618 20:30）
```
CA: MSM:SimDAQ:ch1_frames = 10000           ✓
CA: MSM:SimDAQ:running    = 1               ✓  
CA: MSM:Process:ch1_trig_id = 70001         ✓
CA: MSM:Process:ch1_bunch_id = 600001       ✓
REST: PUT /api/SimDAQ/trig_length 2048 → OK ✓
IPCServer: MSG Start/Stop → sent            ✓
caput MSM:SimDAQ:trig_length 2048 → IOC→mem ✓
```

### UI
- PyDM .ui 生成器：`generate_pydm_ui.py` → `msm_daq.ui`
- Web GUI（内建于 RESTServer）：`http://localhost:8080/`
- 开发服务器：`python3 serve_gui.py`

## 运行说明

### 本地
```bash
cd module_daq
cmake -B build -DMSM_ENABLE_EPICS=ON -DMSM_ENABLE_HTTP=ON
cmake --build build
mkdir -p build/config
cp config/app_inittial_config.json build/config/
cp pv_config.json rest_config.json build/

cd build
export LD_LIBRARY_PATH=../module_core/extern/lib:../module_core/extern/epics/lib/linux-x86_64
EPICS_CAS_INTF_ADDR_LIST=127.0.0.1 EPICS_CA_SERVER_PORT=5064 \
EPICS_CA_ADDR_LIST=127.0.0.1 EPICS_CA_AUTO_ADDR_LIST=NO \
./msm_daq_sim
```

### 验证
```bash
# 终端 2：CA 协议
export EPICS_CA_ADDR_LIST=127.0.0.1 EPICS_CA_AUTO_ADDR_LIST=NO
caget MSM:SimDAQ:running
caput MSM:SimDAQ:trig_length 2048

# 启动采集
curl -X POST -d "Start >> ->| SimDAQ" http://localhost:8080/api/msg
curl -X POST -d "Start >> ->| Process" http://localhost:8080/api/msg
sleep 2
caget MSM:Process:ch1_trig_id

# IPC 启动
echo "MSG Start >> ->| SimDAQ" | socat - UNIX-CONNECT:/tmp/msm_daq_sim.sock
```

## 关键文件

| 文件 | 说明 |
|------|------|
| `module_daq/module_sim.h/cpp` | Sim 模块 + SimMem 结构 |
| `module_daq/module_process.h/cpp` | Process 模块 + ProcessMem |
| `module_daq/module_storage.h/cpp` | Storage 模块 + StorageMem |
| `module_daq/main_sim.cpp` | 主入口（含 PVServer/RESTServer 初始化） |
| `module_daq/pv_config.json` | PV 配置（26 PV） |
| `module_daq/rest_config.json` | REST API 配置 |
| `module_daq/msm_dashboard.html` | Web 仪表板 |
| `module_daq/generate_pydm_ui.py` | PyDM .ui 生成器 |
| `module_core/include/PVServer.h` | PVServer API |
| `module_core/src/PVServer.cpp` | PVServer 实现（softIOC fork + CA 双向） |
| `module_core/src/RESTServer.cpp` | REST 服务器（含 /api/msg） |

## 远程部署

```bash
# 复制到远程
scp -r /mnt/hgfs/share/opencode/MSM/module_core zkcx@10.19.54.177:~/opencode/MSM/
scp -r /mnt/hgfs/share/opencode/MSM/module_daq zkcx@10.19.54.177:~/opencode/MSM/
scp -r /mnt/hgfs/share/opencode/MSM/docs zkcx@10.19.54.177:~/opencode/MSM/

# 远程构建（硬件 SDK 可用）
ssh zkcx@10.19.54.177
cd ~/opencode/MSM/module_daq
cmake -B build -DMSM_ENABLE_EPICS=ON -DMSM_ENABLE_HTTP=ON -DMSM_DAQ_ENABLE_HARDWARE=ON
cmake --build build

# 远程运行（硬件模式）
sudo ./build/msm_daq_storage
```
