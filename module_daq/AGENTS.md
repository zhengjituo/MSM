# module_daq — DAQ 采集处理存盘子项目

MSM 框架下的 PXIe 采集系统，管理 1073 采集卡 + 3010 触发卡。本地提供 Sim 仿真模式。

## 目录

```
module_daq/
├── AGENTS.md               # 本文档
├── CMakeLists.txt           # 构建（引用 ../module_core）
├── daq_config.h/cpp         # 配置加载（JSON → 结构化参数）
├── module_sim.h/cpp         # 虚拟数据源（假帧 + 真实模板）
├── module_daq.h/cpp         # 硬件采集（1073 + 3010）
├── module_process.h/cpp     # 数据解析（peek, TrigID/BunchID）
├── module_storage.h/cpp     # 文件存盘（fwrite, 自动 rotate）
├── main_sim.cpp             # SIM 模式入口
├── main_storage.cpp         # 硬件采集入口
├── extern/
│   └── circular_buffer/     # SPSC lock-free ring buffer
├── config/                  # JSON 配置文件
│   └── app_inittial_config.json
├── sdk_include/             # 硬件 SDK (仅硬件模式需要)
└── build/                   # 构建输出
```

## 构建

```bash
cd module_daq
cmake -B build -DMSM_ENABLE_EPICS=OFF    # 无 EPICS
cmake --build build
mkdir -p build/config
cp config/app_inittial_config.json build/config/

./build/msm_daq_sim    # 仿真模式，无需硬件
```

**硬件模式**（远程部署）：
```bash
cmake -B build -DMSM_ENABLE_EPICS=OFF -DMSM_DAQ_ENABLE_HARDWARE=ON
sudo ./build/msm_daq_storage
```

## API 适配说明

所有 MSM_DAQ 模块已从旧 `msm/` API 迁移到新 `module_core/` API：
- `onIdle()` → `handleState({})` 的空 cmd 分支
- `sleep_for(10ms)` → `cv_.wait_for(50ms)`
- `post()` 无唤醒 → `post()` + `cv_.notify_one()`
- IPCServer 新增 `MSG` 命令转发

## 数据流

```
ModuleSim/DAQ → ring buffer → ModuleProcess (peek) → SaveBatchAll MSG → ModuleStorage (fwrite)
```

## EPICS 集成

所有 26 PV 通过 CA 协议可读写。

| 模块 | PV 前缀 | PV 数量 |
|------|---------|---------|
| SimDAQ | `MSM:SimDAQ:*` | 8 |
| Process | `MSM:Process:*` | 13 |
| Storage | `MSM:Storage:*` | 5 |

### 构建（带 EPICS）
```bash
cd module_daq
cmake -B build -DMSM_ENABLE_EPICS=ON -DMSM_ENABLE_HTTP=ON
cmake --build build
mkdir -p build/config
cp config/app_inittial_config.json build/config/
cp pv_config.json rest_config.json build/
```

### 运行
```bash
cd build
export LD_LIBRARY_PATH=../module_core/extern/lib:../module_core/extern/epics/lib/linux-x86_64
EPICS_CAS_INTF_ADDR_LIST=127.0.0.1 \
EPICS_CA_SERVER_PORT=5064 \
EPICS_CA_ADDR_LIST=127.0.0.1 \
EPICS_CA_AUTO_ADDR_LIST=NO \
./msm_daq_sim
```

### 验证 CA
```bash
export EPICS_CA_ADDR_LIST=127.0.0.1 EPICS_CA_AUTO_ADDR_LIST=NO
caget MSM:SimDAQ:trig_length            # 0
caput MSM:SimDAQ:trig_length 2048       # 写配置
caget MSM:SimDAQ:trig_length            # 2048

# 通过 IPC 启动采集
echo "MSG Start >> ->| SimDAQ" | socat - UNIX-CONNECT:/tmp/msm_daq_sim.sock
sleep 2
caget MSM:SimDAQ:running                # 1
caget MSM:Process:ch1_trig_id           # >0 (实时 TrigID)
```

### Web GUI
RESTServer 内建，打开 `http://localhost:8080/` 即显示仪表板。
也可用开发服务器：`python3 serve_gui.py 8081`。

### PyDM 仪表板
```bash
cd module_daq
python3 generate_pydm_ui.py     # 生成 msm_daq.ui
pydm msm_daq.ui                 # 启动 PyDM
```

## EPICS 可选

`module_core` 的 EPICS 依赖通过 CMake 选项控制：
```
-DMSM_ENABLE_EPICS=ON|OFF  (默认 ON)
-DMSM_ENABLE_ZMQ=ON|OFF    (默认 ON)
-DMSM_ENABLE_HTTP=ON|OFF   (默认 ON)
```
