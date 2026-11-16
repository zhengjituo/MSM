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

## 遗留
- 真机双卡回归（合并后未跑硬件；sim 已过，代码层面双方改动区域无重叠）
- EPICS 栈实机联调（softIOC + CA 桥 + PyDM）由用户在 EPICS 环境验证
