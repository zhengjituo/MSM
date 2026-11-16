# Task 022 — 合并 EPICS 分支（main_epics/PyDM）与多卡分支

## 备份 git 号（恢复点）
```
95e83fd  合并前本地 master（多卡全集，task 018/019 + tag v1.1-multicard）
55e6434  合并前远端 origin/master（EPICS 全集，基于 4fee053 分叉）
4fee053  共同祖先（多卡开工前的提交）
5b2604f  合并提交（master，两侧全集）
```
> 回退：`git reset --hard 95e83fd`（丢 EPICS）或 `git reset --hard 55e6434`（丢多卡）。

## 背景
- 多卡 22 个提交（task 018/019）**从未推送** GitHub，只留在本地
- EPICS 扩展（`main_epics/` IPC→CA 桥 + PyDM 界面 + PVServer 改造）在远端
  基于 `4fee053`（多卡之前的点）独立开发，8 个提交（11-14）
- 两条线并行演进，本次三方合并汇合；合并后推送，GitHub 侧补齐多卡

## TODO
- [x] T1 fetch + 评估分叉（远端 8 提交 vs 本地 22 提交，共同祖先 4fee053）
- [x] T2 解决 4 处冲突（README / main_hwdaq.cpp / html / module_process.cpp）
- [x] T3 任务编号去撞号（EPICS 侧 018/019 → 020/021）
- [x] T4 全量构建（含新目标 msm_epics / msm_cli_config）
- [x] T5 simsrc 冒烟（Status 自动导出 + 采集 + 干净退出）
- [x] T6 推送 master + tag v1.1-multicard
- [x] T7 真机双卡回归（473 批零丢失、dist/相位全中）
- [x] T8 修 pydm 启动（venv 路径失效 + PyQt5 uic channel 属性兼容）
- [x] T9 修 run_epics.sh 被用户环境 EPICS_BASE 劫持（桥加载失败静默退出）

## 冲突与裁决
| 文件 | 双方改动 | 裁决 |
|---|---|---|
| README.md | 我们：多卡口径补充；对方：整篇重写（AI 框架定位+截图） | 取对方新版，回填多卡口径（1–4 卡 / 4–16 通道，2 处） |
| main_webui/main_hwdaq.cpp | 对方：saved_frames→consumed_frames 改名；我们：ChannelStatus 增 slot | 两者都保留（新字段名 + slot） |
| web/msm_daq_hw.html | 对方：同上改名；我们：通道表按卡分组 + slot 表头 | 我们的分组结构 + 对方字段名 |
| module_process.cpp | 对方：删手动 Status 分支（基类 fillStatus 自动导出 VarTable + onStatus 钩子）；我们：新增 SetFrameIndex / EnableSave / DisableSave 分支 | 删 Status（基类 runLoop 拦截后 handleState 不可达，留了是死代码），保留我们三个控制分支 |

语义核对（自动合并区）：
- module_daq.cpp：对方删 handleState 的 Status 分支 + 我们每卡读线程，无重叠
- module_process extractFrame：对方 lastMin_/Max_/SumL_/Vpp_ + 波形快照，
  我们 lastX_/Y_/I_，双方都保留
- main_epics 控制通道（set_frame_index/save_enable）走 io 变量/SetVar 消息，
  与我们 webui/CLI 的消息路径并行，互不冲突

## 决策
- EPICS 侧任务文件 `018_pv_publish_throttle.md` / `019_pydm_control_ui.md`
  重编为 **020 / 021**：10 月的 018/019（多卡）已合入打 tag 且被交叉引用，
  后写的让号。仅改文件名与各自 H1，无其他交叉引用
- 推送范围：master 全量 + tag `v1.1-multicard`（补推，tag 指向 6358e24 代码里程碑）

## 验证
- 构建：`cmake --build build -j 8` 全过，新增 `msm_epics`、`msm_cli_config` 首次编译通过
- simsrc 冒烟（8ch 配置，start→12s→stop→quit）：
  - IPC `Status >> -@ Process`：基类 VarTable 自动导出（chN_min/max/sum_l/
    sum_h/vpp）+ onStatus 扩展（chN_frames/batches/trig_id/bunch_id、
    chN_feat_*、wave_raw base64、x_pos/y_pos/intensity、frame_index、batch_no）齐全
  - 8ch×920000 帧 DAQ=Process 零丢失；Storage=0（startStorage=false 门控生效）
  - 无 readpass gap、无报错，IPCServer 干净退出

## 日志
- 2026-11-16 用户：从 GitHub 更新最新版（其 11-14 已推送 EPICS 扩展）→
  fetch 发现远端 master 基于 4fee053，与本地多卡分叉；用户确认多卡之前
  未推送（本 task 起因）
- 2026-11-16 三方合并 5b2604f，4 冲突按上表裁决；sim 冒烟通过；
  推送 master + tag
- 2026-11-16 **真机双卡回归（合并后）**：8ch×473 批，批间差恒 100000
  （零丢失）、dist=0 473/473、相位 0µs（ch2–8 相对 ch1）、8 通道帧数
  完全一致（4,730,536）；readpass gap 仅启动 pass 2–4 共 4 次（115–215ms，
  32MB 缓冲吸收场景，未丢帧），稳态无 gap，干净退出
- 2026-11-16 用户报"找不到 pydm"：run_pydm.sh 默认 venv 在
  /tmp/opencode/pydm-venv（/tmp 清理后消失）。系统实际已装 pydm 1.13.0 +
  epics 3.5.9 + PyQt5 5.15.3（~/.local）。修两处：
  1. run_pydm.sh：PYDM 默认改为 PATH 查找（$PYDM > pydm > 旧 venv 路径），
     PYBIN 缺失时回退 python3
  2. gen_pydm.py / msm_daq.ui：channel 属性加 stdset="0"——PyQt5 uic 对
     非标属性生成 setChannel()（PyDMLabel 无此方法，offscreen 实测
     AttributeError）；stdset="0" 走 setProperty，pydm 的 channel 是
     @Property(str) 真 Q_PROPERTY，setProperty 触发 Python setter 验证通过。
     curves 不加（PyDMWaveformPlot.setCurves 真实存在，官方为 .ui 设计）
  offscreen 冒烟：msm_app.py 主窗口（控制条 + 内嵌 Display）加载成功
- 2026-11-16 用户报"run_pydm.sh 后 run_epics.sh 退出了"：实为**桥从未起来**
  （脚本 `wait $EPICS_PID` 等桥，桥 loader 报错秒退，脚本跟着退）。
  根因：用户 .bashrc 导出 `EPICS_BASE=~/env/base-7.0.5` + LD_LIBRARY_PATH 注入，
  脚本 `EPICS_BASE="${EPICS_BASE:-...}"` 优先用了用户环境 → 桥需要 vendored
  树的 libCom.so.3.23.1 / libca.so.4.14.4，base-7.0.5 只有 3.19.0 / 4.13.8
  → `error while loading shared libraries`。修 run_epics.sh：
  1. 固定 vendored 树（`MSM_EPICS_BASE` 才允许外部覆盖）
  2. 起桥前 ldd 预检，缺库明确报错
  3. 桥异常退出时打印退出码 + 指向 /tmp/msm_epics.log
  验证（在带坏 EPICS_BASE 的环境里跑）：桥 62/62 PV 连接，caget 读出
  CTRL:backend_alive/daq_running=Off、Process/DAQ:ch1_frames=0，链路全通

## 遗留
- EPICS 栈桌面联调：本 shell 无 DISPLAY，PyDM 只做了 offscreen 加载验证；
  完整链路（run_epics.sh 起 softIOC+CA 桥 → 桌面 run_pydm.sh 看界面）
  由用户在 RDP 桌面跑
