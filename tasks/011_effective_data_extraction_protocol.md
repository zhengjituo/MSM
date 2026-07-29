# Task 011 — 修正 EffectiveDataExtraction 参数格式 (hex→decimal) + WorkMode 支持

## 起始 commit
```
20779ea fix: WorkMode string->int mapping (ref Qt KWorkModeMap) so SDK receives number not string
```

## TODO
- [x] 定位根因：新固件 SDK 的 EffectiveDataExtraction 参数改用纯十进制字符串，而非 hex fixed-point
- [x] `module_daq.cpp` `updateConfig()`: Kx/Ky/K/OffsetX/OffsetY/K1st/B1st 改为直接发配置原值（十进制）
- [x] `frame_parser.cpp`: 确认 I 公式已对齐 Qt 参考代码，无需改动
- [x] 修复 config 中误改的 K/Kx/Ky/B1st/ValueThresholdHigh/Low 值
- [x] 支持 `sum_th_h`/`sum_th_l` WorkMode 枚举映射
- [x] 同步 `msm_webui` 编译
- [x] 硬件验证 X/Y/I 值恢复正常

## 日志

### 根因
`updateConfig()` 写 EffectiveDataExtraction 参数时使用了 `fixedPointHex2()`（输出 `"0x00010000"`），但新固件 SDK 按纯十进制字符串解析，导致 K=65536 而非 1.0，硬件内部 X/Y/I 计算全部错误。

### 修正
- Kx/Ky/K/OffsetX/OffsetY: `fixedPointHex2(std::stod(...))` → 直接发 `chCfg.kx.c_str()` 等配置原值
- K1st/B1st: `fixedPointHex3(std::stod(...))` → 直接发 `chCfg.k1st.c_str()` 等配置原值
- 对齐新版 Qt 代码（`program.new/`）的 `QString::number()` 做法

## 结束 commit
```
8027f47 fix: updateConfig sends EffectiveDataExtraction params as decimal (new SDK protocol)
```
