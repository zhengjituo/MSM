# Config 编辑器合并进 msm_webui 设计

日期：2026-08-14
状态：已确认

## 背景与动机

webui 的 config 入口当前用独立进程 `msm_config_editor`（8081 server）+ 进程管理
（fork/launch/stop/SIGKILL 兜底/editor_alive 状态同步）。该方案引入多类 bug：

1. **close 按钮竞态**：`stopConfigEditor`（HTTP handler 线程）与 `checkEditorChild`
   （poll 线程）无锁并发读写 `g_editorPid/g_editorAlive`。编辑器已退出时 waitpid 返回
   ECHILD，3s 后仍执行 `kill(g_editorPid, SIGKILL)`——pid 可能已被系统复用，误杀无辜进程。
2. **UI 状态错乱**：编辑器退出但 `editor_alive` 未及时清 → Config 按钮灰、Close 使能、
   running 灯亮，均与真实状态不符。
3. **运行中保存保护失效**：`probeDaqRunning` 字符串匹配 `"running":true`（布尔），但
   `Json::dump()` 输出 `"running": "true"`（字符串）——三套模式全不匹配，`acquiring_`
   恒为 false，保存永不拦截。
4. **桌面崩溃**：日志显示两次会话级登出（GDM session closed → systemd SIGKILL 全部
   用户进程），webui/editor 均为受害者，非 close 代码直接触发；合并后可减少一个进程与
   一类变数。

## 设计目标

- 删除 `msm_config_editor` 独立进程与 8081 server，编辑器状态机并入 webui 进程
- 删除进程管理全链路与 acquiring 门控（运行中可打开/保存，保存只写文件）
- reload 剥离 Stop 序列，仅空闲（非采集）时可用
- 保存/加载全部经 MessageBus 消息（保留状态机接口完整性）

## 架构

```
msm_webui (8080)
├── httplib Server（现有）
│   ├── /api/config/meta      → GET，静态元数据（枚举/可编辑键/卡片标签/版本/路径）
│   ├── /api/config           → GET，GetConfig 消息 → config_json
│   ├── /api/config/save      → POST，SaveChannel/SaveCard/SaveChassis → status
│   ├── /api/config/save-app  → POST，SaveApp → status
│   └── /api/config/reload    → POST，经现有 IPC 发 RELOAD 给 hwdaq
├── MessageBus::instance()（webui 进程内独立总线）
│   └── ConfigEditorModule（registerModule + start）
│       ├── cfg_ 内存态 / loadedPath_
│       ├── applyEdit / applyAppEdit / applyAppEditToFile / getConfigJson
│       └──（删除）probeLoop / probeDaqRunning / acquiring_ / /api/lock
└── IPCClient（现有，连 /tmp/msm_daq.sock，与总线互不干扰）
```

## 组件与改动

### 1. CMake（main_webui/CMakeLists.txt）
- `target_link_libraries(msm_webui PRIVATE module_config cpp-httplib pthread)`
  - module_config 已 PUBLIC 依赖 msm_core，需确认 include 路径传递
- 删除 main_config_editor 构建目标（根 CMakeLists 移除引用）

### 2. main_webui/main_hwdaq.cpp
- 删除：`findConfigEditorPath` / `launchConfigEditor` / `stopConfigEditor` /
  `checkEditorChild` / `g_editorPid` / `g_editorAlive` / `editor_alive` SSE 字段 /
  `/api/config`(POST 拉起) / `/api/config/close`
- 新增：全局 `ConfigEditorModule g_configEditor;`
- main 中：`bus.registerModule(&g_configEditor); g_configEditor.start();`
  - 注意：webui 进程需初始化 `ConfigEditorModule` 的 loadedPath_（resolveConfigPath 逻辑，
    基于 exe 定位 `../../config/app_runtime_config.json`）
- 新增端点（httplib handler 内用 `MessageBus::sendAndWait`）：
  - `GET /api/config/meta`：静态返回 editableChannelKeys / enumOptions / cardTypeLabels /
    version / runtimeFile / initFile
  - `GET /api/config`：发 GetConfig，返回 config_json
  - `POST /api/config/save`：解析 scope/cardIndex/channel/params，发 SaveX 消息，回 status
  - `POST /api/config/save-app`：发 SaveApp；saveToInit 时调 applyAppEditToFile（方法调用，
    因为 initPath 不在模块内）
  - `POST /api/config/reload`：`ipc.send("RELOAD")`（现有 /api/msg reload 同路径）

### 3. module_config/config_editor_module.{h,cpp}
- 删除：`acquiring_` / `probeLoop` / `probeDaqRunning` / `probeThread_` / `running_`
  / `registerVars` 的 acquiring 声明 / `getAcquiringJson` / `isAcquiring` / `GetLock`
- `start()` 去掉 probe 线程启动；`stop()` 去掉 join
- `applyEdit` / `applyAppEdit` / `applyAppEditToFile` 删除 `if (acquiring_.load()) return "locked"`
- handleState 保留（消息驱动；webui 用 sendAndWait），删 GetLock 分支
- 头文件注释同步更新

### 4. main_webui/web/msm_daq_hw.html
- 删除：⚙ Config / ✕ Close 按钮、editorIndicator、editor_alive 渲染分支
- 新增 Config 面板（tab 切换 dev/app，移植 config_editor.html 的 UI）：
  - 卡片下拉 / 通道下拉 / 通道参数网格（含枚举 select）
  - runEnable 勾选框（唯一入口，参数网格跳过 runEnable）
  - app_config 键值网格 + 「保存到启动配置」勾选
  - 保存按钮 → POST /api/config/save*，toast「已保存，reload 后生效」
  - Reload 按钮 → POST /api/config/reload，`disabled = d.running`
- 面板可任何状态打开；保存永不因采集被禁

### 5. main_cli/main_hwdaq.cpp
- reload 分支：删除 `Stop >> -@ DAQModule/Process/Storage` 序列，仅保留
  `loadFromJson(g_runtimeConfigPath)` + Distribute + 提示 start
- 门控：由前端 `d.running` 保证 reload 仅空闲可用（双保险：后端 reload 前置检查
  acquiring 非采集才执行，可选）

### 6. 清理
- 删除 `main_config_editor/` 目录、web/config_editor.html（内容并入 msm_daq_hw.html）
- 根 CMakeLists.txt 移除 main_config_editor

## 数据流

**保存**：前端 → POST /api/config/save → sendAndWait(SaveChannel) → ConfigEditorModule
handleState → applyEdit（校验 + 原子写 runtime）→ Response(status) → HTTP 200/409

**生效**：前端 Reload（空闲时）→ POST /api/config/reload → IPC RELOAD → hwdaq
loadFromJson(runtime) → Distribute → 提示 start → 用户 Start

**运行中保存**：直接写文件，不触碰当前采集；下次 reload + start 生效。

## 错误处理

- sendAndWait 超时（>5s）→ HTTP 500，toast「编辑器无响应」
- applyEdit 返回 error:* → HTTP 400，透传 msg
- config 文件不可读 → GET /api/config 返回 404 + toast
- reload 时 hwdaq 非空闲 → 后端前置检查拒绝（返回已采集提示），前端门控兜底

## 测试策略

1. 编译：msm_webui / msm_cli_hwdaq / module_config 全过
2. 单测/脚本：GET meta / GET config / save channel / save app / reload 空转
3. 集成（sim 模式）：
   - 打开面板 → 改参数 → 保存 → 文件 diff 正确
   - reload 后 start → 新参数生效
   - 采集运行中打开面板 + 保存 → 保存成功且采集不受影响
   - reload 门控：运行中不可点，空闲可点
4. 回归：原有 start/stop/save/waveform/status 不受影响

## 边界与已知限制

- 结构性配置改动（通道数/卡类型/runEnable）需 stop + reload + start（ring 在 main 固定，
  无法热重建）
- webui 与 hwdaq 是两套独立 MessageBus，通过 socket IPC 桥接；ConfigEditorModule 只在
  webui 总线内，不直接连 hwdaq
- reload 为「剥离 Stop」语义：仅空闲可用；不提供运行中热更新
