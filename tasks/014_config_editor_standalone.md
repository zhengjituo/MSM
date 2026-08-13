# Task 014 — 独立 config 编辑器（module_config + msm_config_editor）

## 备份 git 号（恢复点）
```
9774ca6  docs: config editor spec - ConfigEditorModule on MessageBus, internal DAQ gating, quadratic params follow Qt
```
> 若阶段 A 开发后需回退，`git reset --hard 9774ca6` 即可恢复原程序状态。

## 目标（独立 MSM 状态机，最后合并）
- 独立小工具：import config from json file → 解析 → edit → export to json file
- 与原程序交集最小化，仅：
  1. config 按钮 + 运行状态监视的启动调用
  2. 配置文件 json 的读写交互
- 阶段 A：只改 module_config/、main_config_editor/、根 CMakeLists，不触碰 module_storage、main_cli、main_webui 现有代码
- 阶段 B（后续）：webui 入口按钮、module_storage 磁盘安全（P0-B）、main_cli 迁移逻辑复用、验证

## 设计要点（见 spec 9774ca6）
- `ConfigEditorModule : msm::Module` 挂独立 MessageBus；HTTP handler 用 sendAndWait
- 内部 ~2s 探测 DAQ IPC（`MSG Status >> -@ DAQModule`）→ acquiring_；采集中保存返回 409
- 原子写 tmp.<pid> + fsync + rename；版本迁移 .bak + 增量补默认
- JSON 兼容：ordered_json 往返保键序/未知字段/值类型

## TODO
- [ ] T1 module_config 骨架（ConfigEditorModule + load + getConfigJson）
- [ ] T2 原子读写（tmp+fsync+rename；ordered_json 往返）
- [ ] T3 版本迁移（ConfigVersion + migrate.cpp + .bak 备份）
- [ ] T4 保存语义（channel/card/chassis 三种 scope + 已知键集合校验 + enum 校验）
- [ ] T5 内部 DAQ 门控（IPC 探测 acquiring_；无 socket 视为未运行）
- [ ] T6 main_config_editor（薄 main + REST 全接口 + 优雅退出）
- [ ] T7 前端 config_editor.html（两 tab + 枚举映射 + 3 保存按钮 + 门控 + 退出）
- [ ] T8 单元测试（往返/scope/枚举/迁移/门控）
- [ ] T9 手工集成验证
- [ ] T10 构建接入（根 CMakeLists add_subdirectory）
- [ ] T11 提交 + 完结 task

## 日志
- 2026-08-13 阶段 A 完成：
  - T1~T5 `module_config/config_editor_module.{h,cpp}`：ConfigEditorModule 独立 MSM 状态机；
    import/export（原子写 tmp.<pid>+fsync+rename）、版本迁移（ConfigVersion=1，.bak 备份 +
    增量补默认）、三种 scope 保存（channel/card/chassis，跳过 3010）、已知键 + 枚举 + 身份键校验、
    DAQ 门控探测（/tmp/msm_daq.sock 的 MSG Status，无 socket 视为未运行）
  - T6 `main_config_editor/main_config_editor.cpp`：薄 main + httplib REST（/api/config、
    /api/lock、/api/meta、/api/config/save、/api/config/save-app、/api/shutdown），
    支持 `--config <path>` 参数（便于以 backup json 为中介测试），优雅退出
  - T7 `main_config_editor/web/config_editor.html`：两 tab（设备参数/软件参数）、枚举下拉映射、
    3 个保存按钮、门控置灰、退出按钮
  - T8 `tools/test_config_editor.py`：端到端验证 18 项 **ALL PASS**（读入 backup → 编辑 →
    写回 backup → 恢复原状）；期间修复 2 个 bug：
    - `cardTypeLabels()` 与匿名命名空间函数重名导致无限递归崩溃（改名为 *Impl）
    - meta JSON 字面量 `{"keys":[]` 少一个 `[`
  - T10 根 CMakeLists add_subdirectory(main_config_editor)；编译全绿

### 设计决议
- 编辑器完全独立；与原 DAQ 程序交集 = config 按钮 + 运行状态监视 + 配置文件读写
- 以 `config/app_initial_config_backup.json` 为中介测试，不污染主配置文件
- 调试方式：gdb `catch throw` 捕获无限递归（栈溢出）；python urllib 替代缺失的 curl

## 结束 commit
```
4d25c91  feat: independent config editor (Phase A) - ConfigEditorModule + msm_config_editor REST + webUI
```
阶段 A（独立 config 编辑器）完结。阶段 B（webui 入口按钮、main_cli 迁移复用、module_storage 磁盘安全）待后续。
