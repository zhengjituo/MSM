# Context: WebGUI Restore - 2026-07-09

## 当前状态
- module_process 已加 `declareVar`: ch1~4 的 trig_id/bunch_id/frames/batches
- main_storage 每秒写 `/tmp/msm_live.json`（含 trig_id/bunch_id 等）
- rest_bridge.py 提供 REST API + 静态文件服务
- e2e_dashboard.html 有 StartAll/StopAll/Recording 按钮 + 4通道 TrigID/BunchID 显示
- 编译通过，msm_daq_storage 可运行

## 启动方式（终端1）
```bash
cd /home/zkcx/ws/MSM_DAQ
LD_LIBRARY_PATH=module_core/extern/lib:module_core/extern/epics/lib/linux-x86_64 \
EPICS_CAS_INTF_ADDR_LIST=127.0.0.1 EPICS_CA_SERVER_PORT=5064 \
EPICS_CA_ADDR_LIST=127.0.0.1 EPICS_CA_AUTO_ADDR_LIST=NO \
./build/msm_daq_storage
```

## 启动 Web 服务器（终端2）
```bash
cd /home/zkcx/ws/MSM_DAQ
python3 module_daq/rest_bridge.py
# 浏览器打开 http://localhost:8088/
```

## 调试命令
```bash
# 查看进程
ps aux | grep msm_daq

# 检查 JSON 数据
curl http://localhost:8088/api/data

# 发送命令
curl -X POST -d "Start >> ->| Process" http://localhost:8088/api/msg
curl -X POST -d "Stop >> ->| DAQModule" http://localhost:8088/api/msg
curl -X POST -d "Start >> ->| Storage" http://localhost:8088/api/msg
curl -X POST -d "Stop >> ->| Storage" http://localhost:8088/api/msg

# 查看实时日志
tail -f /tmp/daq.log
```

## 修改过的文件
- module_process/module_process.h - 加 trig_id/bunch_id atoms + getters
- module_process/module_process.cpp - 加 registerVars + 存帧摘要到 atoms
- module_main/main_storage.cpp - 加 writeLiveJSON 每秒写 /tmp/msm_live.json
- module_daq/rest_bridge.py - 新增 REST API 桥接
- module_daq/e2e_dashboard.html - 新增 Web GUI

## 后续可做
- 加 SUM/X/Y/Intensity（等帧格式确认后）
- 数组/波形显示
- 配置文件保存开关
- 自动启动脚本

## 本地参考代码
- /home/zkcx/ws/MSM_DAQ/module_calculator/ 有 AddModule/MultiplyModule/WaveModule 的完整实现
- /home/zkcx/ws/MSM_DAQ/module_calculator/msm_dashboard.html 有完整 Web UI
- /home/zkcx/ws/MSM_DAQ/docs/design.md 有 MSM 框架设计文档
