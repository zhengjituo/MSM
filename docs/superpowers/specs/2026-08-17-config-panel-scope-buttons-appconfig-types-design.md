# Config 面板恢复 scope 按钮 + app_config 类型化渲染 Design

状态：已实施（2026-08-17）

## 背景动机

config 编辑器合并进 webui 时（commit e6eb18f），前端面板相对旧 `config_editor.html` 有两个功能退化：

1. **scope 保存按钮丢失**：旧版有三个保存按钮（当前通道 / 当前板卡全部通道 / 机箱全部 1073 通道），
   新面板只有一个「保存」按钮且 `doCfgSave` 硬编码 `scope: 'channel'`，无法一次配置整卡/整机箱。
2. **app_config 布尔量渲染退化**：旧版按 JSON 类型渲染（bool→checkbox、number→number、string→text，
   跳过 ConfigVersion）；新面板 `cfgRenderApp` 全部渲染为 text input，布尔量（如
   `autoAcqInEpicsWhenSoftwareStarts`、`isEpicsControlMode`、`startStorage`）容易被误填为字符串。
3. **滚动条**：`cfgParamGrid`/`cfgAppGrid` 带 `max-height` + `overflow:auto`，只显示一屏故出现滚动条。

后端无需改动：`applyEdit`（config_editor_module.cpp）已支持 channel/card/chassis 三 scope，
chassis 跳过 3010 时钟卡；`/api/config/save` 已按 `scope` 分发（main_hwdaq.cpp Task 4）。

## 改动（全部在 `main_webui/web/msm_daq_hw.html`）

### 1. 三个 scope 保存按钮

- 面板顶部按钮区（现 `btnCfgSave`「保存」）替换为三个按钮：
  - `保存当前通道` → `cfgSave('channel')`
  - `保存本卡全部通道` → `cfgSave('card')`
  - `保存机箱全部 1073 通道` → `cfgSave('chassis')`
- `doCfgSave()` 重构为 `cfgSave(scope)`：复用当前面板网格值（`cfgCollectParams`），仅 scope 不同。
  成功后 toast + `cfgLoad()`。

### 2. app_config 完整类型化渲染

- `cfgRenderApp` 对齐旧版 renderAppGrid：
  - 跳过 `ConfigVersion`（只读，系统维护）
  - `typeof val === 'boolean'` → checkbox（`data-jtype="bool"`）
  - number / 数字字符串（无 `_type` 后缀）→ number input（step any，`data-jtype="number"`）
  - 其余 → text（`data-jtype="string"`）
- `cfgSaveApp` 收集时按 jtype 还原：bool→`el.checked`、number→`parseFloat(el.value) || 0`、string→`el.value`
- 排除 `ConfigVersion`，避免保存只读键

### 3. 滚动条移除

- `cfgParamGrid`：删除 `max-height:340px; overflow:auto`
- `cfgAppGrid`：删除 `max-height:200px; overflow:auto`
- 内容全部展开，页面自然滚动

## 测试策略

- 前端验证：webui（sim 后端）冒烟，`cfgSave('card')`/`cfgSave('chassis')` body scope 正确；
  app_config 布尔字段渲染为 checkbox；刷新后值类型保持
- 不涉及跨模块接口/配置格式/数据存储变更，无额外确认需要

## 非目标（YAGNI）

- 不改后端（applyEdit/端点/chassis 跳过已就绪）
- 不改 config_editor_module 的状态机接口
- 不做 channel/card/chassis 之外的第四 scope