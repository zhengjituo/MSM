# Task 023 — PyDM 多卡 tab + 波形/退出三个隐性 bug

## 目标

1. PyDM 对齐 webui 的多卡形态：本机插 2 卡（8 通道），界面按卡分 tab
2. 回答并修正"Frame 显示 0"的困惑（frame_index 语义 + 负值被钳）
3. 帧数表 "Storage" 列改名 "Consumed"（消耗帧数 ≠ 已存盘帧数）
4. 修掉用户反馈的退出期 `RuntimeError: wrapped C/C++ object of type
   Connection has been deleted`（每个 PV 一条，50+ 条刷屏）

## 改动

### 多卡（pv_map.json 是唯一真源）
- `config/pv_map.json`：slow 组补 ch5-8（DAQ:ch5-8_frames、
  Process:ch5-8_{frames,batches,trig_id,bunch_id,min,max,sum_l,sum_h,vpp}、
  Storage:ch5-8_consumed_frames，46→90 条）；wave.channels [1-4]→[1-8]
- `main_epics/gen_pydm.py`：`--nchannels` 默认改为**从 pv_map 自动推导**
  （Process:chN_frames 与 wave channels 的最大值），卡数变了不用手改；
  仍可用 `--nchannels` 覆盖
- `main_epics/msm_pvs.db` 重新生成：62→110 条记录
- 桥 main_epics.cpp **零改动**（本来就是按 pv_map 通用映射，后端
  module_process/daq/storage 早已按全局 ch1..chN 暴露）
- 仿真栈验证：110/110 PV 连接；DAQ:ch5/ch8_frames 递增、
  Process:ch8_vpp=1.18、wave_ch5 NORD=4096 有数据
- 注意：EPICS 侧是静态生成（.db/.ui 是派生物），换卡数 = 改 pv_map
  重跑 run_epics.sh（自动重新 gen_db）+ run_pydm.sh --regen

### Index/Set 挪到顶部状态栏
- frame_index 是**全局**变量（batch 相位对齐对所有卡同义，后端只有
  一个 frameIndex_），原实现每卡一个 Index/Set 且同名
  （editFrameIndex/btnSetFrameIndex），Qt findChild 只能连上第一个 →
  第 2 卡的 Set 是死的。对齐 webui（batch #N idx=M + Set 在顶部）：
  移入顶部状态栏，全界面唯一入口；msm_app.py 的 findChild 接线不变
- 语义：-1 = 自动对齐 100ms 边界（bunch_id 末尾 00000）；
  ≥0 = 固定选 batch 内第 N 帧（0 = 该 batch 第一帧）
- "Frame 显示 0" 的由来：旧 .db 的 ai 记录没设 raw 钳制（LOPR/HOPR
  默认 0），自动模式的 -1 被钳成 0 显示（022 已修 LOPR/HOPR=±1e9）；
  另外用户之前测试时确实 Set 过 0（桥日志 value=0）。现构建自动模式
  显示 -1

### Consumed 改名
- gen_pydm.py 帧数表头 ["CH","DAQ","Process","Batches","Storage"] →
  "Consumed"；main_webui/web/msm_daq_hw.html 同列同改
- PV 名不动（Storage:chN_consumed_frames 是模块命名空间 + 本就带
  consumed；改名要动桥/后端/三处消费者，不值）

### RuntimeError（退出期）—— 复现 + 定位 + 修复
- 复现：offscreen 驱动（build→exec→关窗→进程退出），stderr 出现 50+
  条 "Unable to remove connection ... wrapped C/C++ object of type
  Connection has been deleted"
- 定位（插桩）：exec 返回后、帧弹出前 plugin/连接表/C++ 全部健康
  （dict=50 alive=50 dead=0）；错误全部发生在**解释器 finalize 阶段**：
  CPython 按模块序清理，PyQt5/sip 先批量删 Connection 的 C++ 对象，
  PyDM 控件的 destroyed 信号回调稍后才跑 channel.disconnect() →
  remove_connection() 对已死 C++ 调 deleteLater() → 炸。rm_calls=1、
  dict_n 从 50 递减，排除重复调用/共享连接
- 修复（msm_app.py，不动 site-packages）：main() 里 exec_() 返回后、
  进程退出前显式 teardown：
  1. pydm.utilities.close_widget_connections(win) 正常断开全部通道
  2. 兜底：逐个 Connection.close() 后清空 plugin.connections/channels
  退出时 destroyed 回调里 remove_connection 全部空操作
- 回归：offscreen 15s 全量跑，错误 50+ → 0

### 附带发现并修复
- **波形从未真正渲染**：pydm 1.13.0 的 waveformplot/scatterplot/
  timeplot 用 `np.float`（numpy≥1.24 移除，本机 2.2.6）→ 每次重绘
  抛 AttributeError（15s 内 5628 条）。msm_app.py 顶部补
  `numpy.float = float` 兼容 shim（该别名本就等价内建 float，只影响
  本进程）。回归：0 条
- **run_epics.sh kill_stale 竞态**：TERM 后进程自行退出时，KILL 循环
  对已死 pid 返回非零 → `set -e` 静默杀脚本（本轮实测踩中）。
  `kill -KILL ... || true`

## 验证
- 仿真栈 110/110 PV 连接；ch5-8 计数器/vpp/waveform 全部有数
- offscreen：2 tab（Card 1 CH1–CH4 / Card 2 CH5–CH8）、顶部
  Index/Set 存在、np.float 0 条、Connection 错误 0 条、干净退出
- 待用户在 RDP 桌面确认双卡真机显示

## 遗留
- 真机 2 卡的 PyDM 目检（tab 切换、CH5-8 波形/特征表、Set -1/500）
- 若以后换 3/4 卡：pv_map 加到 ch12/16 即可，gen 全链路自动
