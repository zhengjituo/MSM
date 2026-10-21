# 多卡采集支持设计（最多 4×1073，严格 slot 匹配）

日期：2026-08-24　状态：已确认（用户 2026-08-24 拍板）

## 背景与目标

当前代码单卡可用。目标：扩展到最多 **4 张 1073**（4 通道/卡 = 16 通道），
DAQ / Process / Storage / WebUI 全链路支持。

核心语义：**打开数量 = 配置文件 slot ∩ 机箱扫描 slot，严格匹配**。
缺卡或槽位不符 → 报错 + 日志（便于区分硬件故障与配置错误），不进入采集。

## 已具备的基础（不需要重写）

| 位置 | 现状 |
|---|---|
| `module_daq.h` | `MAX_CH = 16`；`cards1073_` 向量；`activeChannels_[16]` |
| `module_daq.cpp` | `initHW` 扫描并按 slot 排序；`updateConfig` 按配置 `card.slot` 匹配扫描卡 |
| `config_impl` | 多卡 JSON 解析/序列化；`CardCount` 分发 |
| `main_*.cpp` | ring 数量 = 配置 `runEnable` 通道总数 |
| `module_process` / `module_storage` | `MAX_CH=16`、`nCh_` 动态 |
| `webui` | 通道表/特征表/卡下拉动态；后端 `SaveCard` 已实现 |

## 决策（用户确认 2026-08-24）

1. **映射策略 A（严格 slot）**：只按配置里的 slot 精确匹配，不做"按顺序取用"兜底
2. **缺卡 = 报错**：Init 失败、不采集、进程存活（改配置后可重新 Start）
3. **3010**：配置里有就必须存在，缺失同样报错；配置里没有则不要求
4. **ring 内存**：`app_config.RingSizeMB` 可配，默认 256；4 卡时按 `overflow_count` 调优
5. **通道编号**：按 JSON `Card#N` 顺序（现有行为，不改）；调换卡顺序会改变通道号
6. **配置 UI**：仿原 Qt 程序——当前板卡下拉 + 3 按钮
   （① 保存当前通道 ② 保存当前板卡所有通道 ③ 保存机箱全部）
7. **机箱有但配置没有的卡**：不 open，日志提示忽略

## 关键流程

### Init（每次 Start 都执行）

1. `onReadConfig`——配置进内存
2. 计算期望卡集合 `expected = {1073 slots..., 3010 slot?}`
3. 若 **hw 未就绪** 或 **已打开卡集合 ≠ expected** → `initHW()`：
   - 扫描机箱 → `scanned`
   - 校验 `expected ⊆ scanned`，否则报错（列出缺失 slot + 实际 slots）
   - 只 open+init `expected` 中的卡；机箱多余的卡忽略并 log
4. `updateConfig()`——按配置卡顺序 slot 匹配 → 写寄存器 → 填 `activeChannels_`/ring 映射
5. 回 `Response(status=ok|error, error=原因)`
6. 失败：不配置通道、不采集

### 卡片集合变更（增/删卡）

- 改 pending 增删卡 → 下次 Start 时 `expected ≠ 已打开集合` → 自动
  close+reopen（SDK 不 deinit）
- 集合未变 → 跳过重开，保持现有"秒级热重配"语义

## 错误信息规范

```
[ModuleDAQ] ERROR: 配置要求 1073 slot=5，机箱未找到（实际 1073 slots: 3）
[ModuleDAQ] ERROR: 配置要求 3010 slot=10，机箱未找到（实际 3010: 无）
[ModuleDAQ] ERROR: 配置 1073 数量 5 超过上限 4
[ModuleDAQ] INFO: 忽略未配置的 1073 slot=7
```

## 通道 / ring 映射

- 顺序 = 配置 `cards` 数组顺序 × 卡内通道顺序；`runEnable` 的通道依次分配 ring `0..N-1`
- 全局 CH = ring index + 1
- DAQ Stats / Process / Storage / WebUI 统一用 ring index 作为通道号

## 新参数

`app_config` 新增：

```json
"RingSizeMB": 256
```

- DAQ ring 大小，默认 256；16 通道全开 = 4GB（+ saveRing 256MB）
- 调优依据：WebUI 的 `overflow_count`——无溢出不必调大

## 改动清单

1. `module_daq`：严格校验 + 交集 open + 集合变更重开 + Init 响应错误 +
   保留最近扫描摘要供 UI 查询
2. `main_hwdaq` / `main_simsrc`：Init 改 `sendAndWait` 并检查错误；
   记录 `last_start_error`
3. `config_impl`：`RingSizeMB` 解析/序列化；mains 使用
4. `module_process`：日志去 "4-channel" 硬编码
5. `webui`：波形动态网格（4/8/12/16）；3 按钮；添加板卡 + 扫描 slot 提示
6. 文档：`MSM_DAQ的定义.md`、`module_daq.md`、`README.md`、`AGENTS.md`

## 测试

- 编译四个目标全过
- 仿真：2 卡（8ch）/ 4 卡（16ch）配置跑 SSE / 特征 / 存盘
- 真机：单卡（回归）、双卡；错误路径（配置 slot 不存在、3010 缺失）
- 热路径：单卡配置 → 改双卡配置 → Start 自动重开生效

## 风险

- 4 卡 4GB ring 内存 → `RingSizeMB` 可配
- 单 3010 触发多卡依赖背板 `SyncTrigGlbSync`，需真机验证对齐
- 调换 JSON 卡顺序会改变通道号（已确认接受）
- add-card 需枚举机箱 slot（DAQ Status 暴露扫描摘要）
- 卡片集合变更触发 close+reopen（秒级，比热重配慢）
