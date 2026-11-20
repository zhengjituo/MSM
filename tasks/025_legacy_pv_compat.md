# Task 025 — EPICS 旧版 PV 兼容层（外部设备 ZkcxClient）

## 起始 commit
aeee824

## 背景
旧系统（重写前）= ZkcxIOC（softIoc，`db/ZkcxIOC.db`）+ Qt 值守（`qtProgram/WindowManager/epics_pv_monitor.cpp`）+ 外部设备客户端（`epics_client_software/ZkcxClient.py`）。新版 `main_epics` 命名体系（`$(P)$(I)<模块>:<变量>`，P=`MSM:`）与旧名（`$(P)CARDxCHANNELy后缀`，P=`BE-FEL1-HSS:ADC-1:`）**零交集**，外部设备无法连接。

参考件（仓库外）：
- 旧 db：`/home/zkcx/program/epics_ioc_software/db/ZkcxIOC.db`（666 条记录）
- 旧 IOC 启动：`/home/zkcx/program/epics_ioc_software/iocBoot/iocZkcxIOC/st.cmd`（`epicsEnvSet("P","BE-FEL1-HSS:ADC-1:")`）
- 外部客户端：`/home/zkcx/program/epics_client_software/ZkcxClient.py` + `zkcxWidget.py`
- 旧值守：`/home/zkcx/program/epics_local_software/qtProgram/WindowManager/`

## 用户决策（2026-11-20，勿再问）
1. 兼容目标 = **ZkcxClient.py 用到的 PV 全集**（外部设备，不是旧 Qt 值守的大面）
2. **前缀改成旧值**：P=`BE-FEL1-HSS:ADC-1:`（客户端从自己的 Prefix.cfg 读前缀，客户端零改动）
3. **SAVE 全局语义可接受**（新版上层本就全局开关存储；且 ZkcxClient 根本不用 SAVE，无需实现）
4. 旧 db 是**按 4 卡预备**的（CARDCOUNT 运行时决定实际卡数）→ 兼容层也生成 4 卡全量旧名 PV，CARD3/4 无值时置 0，CARDCOUNT 限住客户端读取范围

## 旧版记录类型（实测自 ZkcxIOC.db；全是 waveform + SCAN=Passive，标量 NELM=1）
| 旧 PV | FTVL | NELM | 说明 |
|:--|:--|:--|:--|
| CARDCOUNT | LONG | 1 | 卡数（客户端启动时读 int） |
| CARD1–CARD4 | LONG | 1 | 卡型号（1073/1012；1012 时客户端隐藏后 3 个波形窗） |
| CARDxOFFSETX / OFFSETY | DOUBLE | 1 | 卡级 → 新是全局 x_pos/y_pos |
| CARDxLIGHT | DOUBLE | 1 | 卡级 → 新是全局 intensity |
| CARDxCHANNELyBUNCHID | UINT64 | 1 | |
| CARDxCHANNELy{MAXVALUE,MINVALUE,HIGHVALUE,LOWVALUE,VPP} | DOUBLE | 1 | HIGH/LOWVALUE 旧义=高/低阈值累加 = 新 sum_h/sum_l |
| CARDxCHANNELyDOWNSAMPLE | LONG | 1 | ns/pt（客户端画 x 轴） |
| CARDxCHANNELy（波形） | **UCHAR** | **1048576** | 推**原始帧字节**：64B 头 + trig_length×2B 采样 + 64B 尾 |
| STARTACQ | DOUBLE | 1 | 写 "1"=启动 / "0"=停止 |
| HANDSHAKE | UCHAR | 1 | 客户端每 2s 写 0（存活）；IOC 侧应周期复位 1 |

帧格式见 `docs/data_formats/frame_format_PD.md`：头 64B（magic EB91@0、包长 U32BE@2、板卡类型 U32BE@7、卡内通道 U16BE@14、slot@16、TrigID U32BE@17、ADC_SCALE U32BE@37、ADC_OFFSET@41、ADC_RANGE@45、PRECISION@49、解码模式@50）+ 采样 + 尾 64B（magic EB92、BunchID U48BE@2、Min U16BE@8、Max@10、Sum_L U32BE@12、Sum_H@16、Vpp U16BE@20、X/Y/I DBL BE@24/32/40 仅 ch1）。

## 外部客户端行为（ZkcxClient.py，兼容验收基准）
- 前缀 = 运行目录 `Prefix.cfg` 全文（客户端零改动，只要我们的 P 与其一致）
- 启动：`int(get_pv_value(CARDCOUNT))` → 卡数下拉与 tab 数（只读前 N 卡的 PV）
- 标签监控：CARDxOFFSETX/Y、CARDxLIGHT、CARDxCHANNELy{BUNCHID,MAX,MIN,HIGH,LOW}VALUE,VPP（选中卡）
- 100ms 定时画波形：`CARDxCHANNELy` 原始帧 + `CARDxCHANNELyDOWNSAMPLE`
- 帧解析（zkcxWidget.set_data）：去头尾各 64B 得采样（每 2B 一点，`(b1<<8)|b2`）；头 `[38:41]`=ADC_SCALE、`[42:45]`=ADC_OFFSET、`[46:49]`=ADC_RANGE，用于 y 轴换算 `y=(value-offset)*range*(pregain/2^20)/65536`；**只读这三段参数**，其余头字段不读
- 特例：128B 帧（EnableWaveForm=0）/ 192B 帧（trig_length=32）
- 写：`STARTACQ` "1"/"0"；`HANDSHAKE` "0" @2s

## 新版事实（本仓库）
- PV 真源 `config/pv_map.json` → `gen_db.py`→`msm_pvs.db`、`gen_pydm.py`→`msm_daq.ui`；`main_epics.cpp` 运行时读同一份 map
- 前缀真源 `config/pre.cfg`（当前 P=`MSM:` I=空）；三个消费者经 `run_epics.sh` 注入（softIoc `-m "P=,I="` / 桥 `MSM_P,MSM_I` / pydm `--macro`）
- 桥架构：主循环轮询 IPC status JSON → `lookupVar` → `caPut`（标量，默认 DBR_DOUBLE / longout DBR_LONG / bi,bo DBR_SHORT）/ `caPutWave`（base64→uint16 样本→DBR_LONG 数组，只写实际长度）；命令侧 CA 订阅（DBE_VALUE）→ 队列 → `ipc.send` → resp 回写
- Process 变量：`chN_{frames,batches,trig_id,bunch_id,min,max,sum_l,sum_h,vpp}`（N=1..8）、`x_pos/y_pos/intensity`（**Ch1 解码的全局单值**）、`batch_no`、`frame_index`；`wave_ch1..8`（4096 uint16 @5Hz，base64 进 status）
- **新版变量只到 ch8**：4 卡（16ch）场景需后端先扩，本轮不做
- 卡级→全局降档：CARD1 精确对应；CARD2–4 重复同值（x_pos/y_pos/intensity 只有一个），已接受

## 设计（兼容层全部落在 EPICS 入口：pv_map.json + gen_db.py + main_epics.cpp，**后端零改动**）
1. `config/pre.cfg`：P=`BE-FEL1-HSS:ADC-1:`（I 留空）——新 PV 随之改前缀，PyDM 自动跟随
2. `pv_map.json` 增 `legacy` 段：`cards=4`、`channels_per_card=4`、映射约定 **N=(x-1)×4+y**
   - 卡级别名：`CARDxOFFSETX`←`Process:x_pos`、`CARDxOFFSETY`←`y_pos`、`CARDxLIGHT`←`intensity`（4 卡全量生成）
   - 通道级别名：`CARDxCHANNELy{BUNCHID,MAXVALUE,MINVALUE,HIGHVALUE,LOWVALUE,VPP}`←`Process:chN_{bunch_id,max,min,sum_h,sum_l,vpp}`
   - 波形：`CARDxCHANNELy`←重构原始帧（见 4）
   - 静态（桥启动写）：`CARDCOUNT`、`CARD1–4`、`CARDxCHANNELyDOWNSAMPLE`×16 ← 读 `config/app_initial_config.json`
   - 命令：`STARTACQ`（1→START / 0→STOP）、`HANDSHAKE`（桥 @1Hz 复位 1 + 记客户端写 0 时间戳）
3. `gen_db.py`：展开 legacy 段 → record 类型与旧 db 完全一致（waveform NELM=1 + 对应 FTVL；波形 UCHAR NELM=1048576）
4. `main_epics.cpp`：
   - `caPut` 增 `uint64`→DBR_UINT64、`uchar`→DBR_CHAR
   - 别名作为 PvEntry 挂进现有组（x_pos/y_pos/intensity→fast，特征→slow，波形→wave），复用 lookupVar/变化检测
   - `reconstructLegacyFrame(ch)`：64B 头（magic、包长、卡型号/slot/卡内通道号/ADC 参数来自 config，TrigID 来自变量，拿不到的时间戳等填 0——客户端不读）+ 采样字节 + 64B 尾（BunchID U48BE、Min/Max U16BE、Sum_L/H U32BE、Vpp U16BE、X/Y/I DBL BE ch1）
   - STARTACQ：新 `CtrlKind CK_STARTACQ`（DBR_DOUBLE 订阅，1→START 0→STOP，不复位）
   - HANDSHAKE：主循环 @1Hz `caPut 1`
   - 启动后写静态 PV（卡数/型号/downsample）
5. 重新生成 `msm_pvs.db`、编译、sim 栈（`main_epics/run_sim_stack.sh`）验收：`caget` 旧名有值、`caput STARTACQ 1/0` 起停后端、`caput HANDSHAKE 0` 被复位、重构帧字节符合客户端解析（头参数三段 + 采样点数）

## TODO
- [ ] pre.cfg 改前缀
- [ ] pv_map.json 增 legacy 段
- [ ] gen_db.py 展开 legacy + 重新生成 msm_pvs.db
- [ ] main_epics.cpp：caPut 类型 / 别名挂组 / 帧重构 / CK_STARTACQ / HANDSHAKE / 静态写
- [ ] 编译 + sim 栈验收（caget/caput 全旧名面）
- [ ] README/docs 是否补一句"EPICS 入口保留旧 PV 兼容层"（待定，先不做）

## 边界 / 不做
- ch9–16（4 卡）：后端 Process 变量未覆盖，本轮不做
- 旧 Qt 值守的大 PV 面（CARDRUN/TRIG*×36 后缀/每通道 SAVE/CARDCONFIG）：目标只是 ZkcxClient，不做
- 不改新版原生命名（module:variable 保留，前缀统一为装置前缀）
