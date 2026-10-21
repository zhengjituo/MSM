# Task 018 — 多卡采集支持（最多 4×1073，严格 slot 匹配）

## 备份 git 号（恢复点）
```
4fee053  docs(plan): config pending-chain implementation plan (task 017)
e026a10  docs(task 018): 设计 + 计划（严格 slot / 缺卡报错 / RingSizeMB）
9b81ed0  T1 feat(daq): 严格多卡校验、只开配置交集、Init 回错误
c0fd613  T3 feat(config): RingSizeMB（默认 256MB）
c737310  T2 feat(cli): Init/Configure 同步错误 + last_start_error，ring 用 RingSizeMB
5f6e9ac  T4 chore(process): 特征表日志按实际通道数
ce2d63b  fix(core): IPCServer stop 时 FD_SET(-1) abort（既有 bug）
efaa0b8  T5/T6 feat(webui): 动态波形网格 + 扫描 slot 提示 + 启动错误显示
37ac93b  T6 feat(config): 1/2/4 卡配置模板（*.json.bak 入库）
8849442  T7 fix(daq): 真机验证修复（TrigSource 映射/清脏状态/错误路径/卡数变化）
a2ea90a  T7 docs: 真机验证结果 + 同步要点 + 卡数变更限制
```
> 回退：`git reset --hard 4fee053`。分支：`feat/multi-card`（主仓库内）。

## 目标
1. 最多 4 张 1073（16 通道），DAQ/Process/Storage/WebUI 全链路支持
2. 打开数量 = 配置 slot ∩ 机箱扫描 slot（严格匹配，不兜底）
3. 缺卡/槽位不符/超限 → 报错 + 日志，Init 失败不采集，进程存活
4. 3010 配置有则必须存在，缺失同样报错
5. ring 大小 `app_config.RingSizeMB` 可配（默认 256MB）
6. 通道编号 = 配置 `Card#N` 顺序 × 卡内通道顺序
7. 配置 UI 仿原 Qt：当前板卡下拉 + 3 按钮（通道 / 当前板卡 / 机箱全部）

设计：`docs/superpowers/specs/2026-08-24-multi-card-support-design.md`
计划：`docs/superpowers/plans/2026-08-24-multi-card-support.md`

## TODO
- [x] T1 module_daq：严格校验 + 交集 open + 集合变更重开 + Init 错误响应
- [x] T2 main 链：Init sendAndWait + last_start_error
- [x] T3 config：RingSizeMB 解析/序列化 + mains 使用
- [x] T4 Process/Storage 文案去 4 通道硬编码
- [x] T5 WebUI 波形动态网格（4/8/12/16）
- [x] T6 配置面板 3 按钮 + 扫描 slot 提示 + 1/2/4 卡模板（不做加/删卡）
- [x] T7 编译 + 仿真多卡验证 + 真机冒烟 + 文档收尾
  - [x] 四个目标编译过
  - [x] 仿真 8ch（2 卡）：DAQ/Process/Storage 各 840000 帧对齐，特征表 8 列
  - [x] 仿真 16ch（4 卡）：16 × 256MB ring，各 350000 帧对齐，干净退出
  - [x] quit/SIGTERM 正常退出（顺带修 IPCServer stop 竞态崩溃）
  - [x] 真机单卡回归（slot3）：703931 帧/8s，4 通道完全对齐
  - [x] 真机双卡（slot3+5，8ch）：约 65 万帧/8s（~80kHz），8 通道
    DAQ/Process 逐通道对齐，3010 触发经背板广播同步
  - [x] 错误路径：配置 slot=7（机箱只有 3/5）→ Init 失败、
    「配置要求 1073 slot=7，机箱未找到（实际 1073 slots: 3,5）」、
    进程存活、start 报 last_start_error、quit 干净退出
  - [x] 单卡→双卡热切换：明确报错「启用通道数变化（4→8）：管线通道数
    固定，卡集合变更后请重启进程」（main 前置检查 + DAQ ring 越界保护，
    原来这里会段错误）

## 验证方式
- 编译：msm_cli_hwdaq / msm_cli_simsrc / msm_ipc_shell / msm_webui
- 仿真：2 卡 8ch、4 卡 16ch 跑 SSE/特征/波形/存盘
- 错误路径：配置 slot 不存在 → 日志列出缺失与实际 slots，不采集
- 真机：单卡回归 + 双卡；单卡配置 → 双卡配置 Start 自动重开

## 日志
- 2026-08-24 设计确认（严格 slot / 缺卡报错 / RingSizeMB / 配置 UI 仿 Qt）；
  本 task 创建
- 2026-10-21 范围收敛：不做加卡/删卡/一键补齐；改为预置
  `config/app_initial_config_{1,2,4}card.json.bak` 模板，按需覆盖 pending
- 2026-10-21 T1–T6 完成并提交（见上），仿真 8ch/16ch E2E 通过；
  修掉 IPCServer stop 竞态导致的 `buffer overflow detected` abort（master 同样复现，
  与本任务无关的既有 bug）
- 2026-10-21 真机验证（机箱 2×1073 slot3/5 + 3010 slot10）：
  - 单卡/双卡均通过（见 T7）；确认同步要点：板卡先 StartChannelRecv，
    3010 TRIG 物理滞后 500ms（现有实现即如此）
  - 关键修复：TrigSource 名字→数值映射（SDK 只认数字；`global`→4，
    多卡同步前提）；首次 Init 全卡 reset 清脏状态（异常退出残留）
  - 修复既有崩溃：`runEnable:false` → `stoi("false")` terminate
  - 修复 main 在首次 Init 失败时 `while(!isHwReady())` 死等 →
    新增 `initFailed_` 标志，进程保持存活可重试
  - 卡集合热切换改为明确报错（下游管线通道数固定，需重启进程），
    替换原来的 ring 越界段错误
- 2026-10-21 触发频率核实（Process 批次日志加墙钟时间戳，`MSM_DAQ_TS=1` 门控）：
  2 卡 8ch 实测触发 100,828/s ≈ 100kHz（略高 0.8% 为 3010 时钟偏差）；
  每批 10,000 帧对应 Δtrig_id 恒为 10,000，**无丢帧**，
  单线程轮询读回 8ch×100k×1152B≈921MB/s 刚好跟得上
- 2026-10-21 WebUI 多卡分组展示（`e2e8e5d`）：DAQ Status 增 `chN_slot`，
  Frame Summary 按卡表头行、Feature Table 每卡一张小表（横向铺开）、
  波形每卡一个分节；sim/无 slot 回退平铺。真机 2 卡验证分组正确
