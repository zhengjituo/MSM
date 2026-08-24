# Config 单链式生效（pending → runtime，无 reload）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 废弃运行中 reload 热生效；webui 只编辑待使用配置（pending），后端下次启动/首次采集 Start 时自主将 pending 原子覆盖到 runtime 再加载。

**Architecture:** 三个文件分工——`app_initial_config.json`（出厂模板，仅首次播种）、`app_pending_config.json`（webui 编辑目标，新增）、`app_runtime_config.json`（后端实际加载）。交接动作由后端进程在启动阶段执行（pending 存在→原子覆盖 runtime→loadFromJson），webui 的 Run 只 fork 后端。彻底移除 reload（webui 按钮/API、CLI 命令、IPC RELOAD）。

**Tech Stack:** C++（main_cli/main_webui/module_config），文件原子写复用现有 `saveToFileAtomic`（tmp+fsync+rename）模式，webui 前端 HTML/JS。

## Global Constraints

- 原子写：交接必须 tmp+fsync+rename，禁止直接写目标文件。
- `app_initial_config_backup.json` 为出厂手动备份，不参与运行，不删除。
- 后端交接后 loadFromJson 失败：回退 initial 播种并明确报错，禁止空配置启动。
- 移除所有 reload 路径：webui Reload 按钮、`/api/config/reload`、`reload` msg、CLI reload 命令、`g_reload`/IPC RELOAD。
- simsrc 与 hwdaq 行为一致（simsrc 额外保持 `slot=0`）。
- 运行中编辑 pending 不得影响后端运行状态（status 轮询正常）。

---

### Task 1: 后端启动交接函数（shared helper）

**Files:**
- Create: `main_cli/config_handoff.h`
- Create: `main_cli/config_handoff.cpp`

**Interfaces:**
- Produces: `int ensureRuntimeConfig(const std::string& initPath, const std::string& pendingPath, const std::string& runtimePath)` — 返回 0 成功、非 0 失败；语义：若 pending 存在则原子覆盖到 runtime；若 runtime 仍不存在则从 initPath 复制到 runtime；成功返回 0。日志经 `std::cerr` 输出。

- [ ] **Step 1: 创建头文件**

```cpp
// config_handoff.h
#ifndef MSM_CONFIG_HANDOFF_H
#define MSM_CONFIG_HANDOFF_H
#include <string>

// 若 pending 存在则原子覆盖到 runtime；若 runtime 仍不存在则用 init 播种。
// 返回 0 成功，非 0 失败。
int ensureRuntimeConfig(const std::string& initPath,
                        const std::string& pendingPath,
                        const std::string& runtimePath);

#endif
```

- [ ] **Step 2: 创建实现文件**

```cpp
// config_handoff.cpp
#include "config_handoff.h"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace {
bool readFile(const std::string& path, std::string& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(ifs),
               std::istreambuf_iterator<char>());
    return true;
}

bool atomicWrite(const std::string& path, const std::string& content) {
    std::string tmp = path + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream ofs(tmp, std::ios::binary);
        if (!ofs.is_open()) return false;
        ofs << content;
        ofs.flush();
        int fd = ::open(tmp.c_str(), O_RDONLY);
        if (fd >= 0) { ::fsync(fd); ::close(fd); }
        else { ofs.close(); ::unlink(tmp.c_str()); return false; }
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}
} // namespace

int ensureRuntimeConfig(const std::string& initPath,
                        const std::string& pendingPath,
                        const std::string& runtimePath) {
    std::string pending;
    bool hasPending = readFile(pendingPath, pending) && !pending.empty();

    if (hasPending) {
        if (!atomicWrite(runtimePath, pending)) {
            std::cerr << "[config] FAILED to copy pending -> runtime: "
                      << runtimePath << "\n";
            return 1;
        }
        std::cout << "[config] pending -> runtime: " << runtimePath << "\n";
        return 0;
    }

    std::string runtime;
    if (readFile(runtimePath, runtime) && !runtime.empty()) {
        std::cout << "[config] no pending, using existing runtime: "
                  << runtimePath << "\n";
        return 0;
    }

    std::string init;
    if (!readFile(initPath, init) || init.empty()) {
        std::cerr << "[config] no runtime and no init seed: " << initPath << "\n";
        return 1;
    }
    if (!atomicWrite(runtimePath, init)) {
        std::cerr << "[config] FAILED to seed runtime from init: "
                  << runtimePath << "\n";
        return 1;
    }
    std::cout << "[config] seeded runtime from init (no pending yet)\n";
    return 0;
}
```

- [ ] **Step 3: 编译验证**

Run: `g++ -c -std=c++17 -I. main_cli/config_handoff.cpp -o /tmp/opencode/config_handoff.o`
Expected: 编译无错误、无警告。

- [ ] **Step 4: Commit**

```bash
git add main_cli/config_handoff.h main_cli/config_handoff.cpp
git commit -m "feat(cli): add config pending->runtime handoff helper"
```

---

### Task 2: main_hwdaq 接入交接 + 移除 reload

**Files:**
- Modify: `main_cli/main_hwdaq.cpp:47-48`（路径常量）
- Modify: `main_cli/main_hwdaq.cpp:84-90`（启动加载）
- Modify: `main_cli/main_hwdaq.cpp:217-232`（CLI reload 分支）
- Modify: `main_cli/main_hwdaq.cpp:242-258`（g_reload 分支）
- Modify: `main_cli/main_hwdaq.cpp:25-45`（g_reload 定义与 IPC RELOAD）

**Interfaces:**
- Consumes: `ensureRuntimeConfig(init, pending, runtime)`（Task 1）
- Produces: 无对外接口。

- [ ] **Step 1: 确认现有文件结构**

Read `main_cli/main_hwdaq.cpp:1-50` 确认 `g_reload` 定义、`handle_ipc`/IPC 命令分发位置、路径常量。

- [ ] **Step 2: 加入 pending 路径常量与 include**

在 `g_runtimeConfigPath` 定义旁新增：

```cpp
static std::string g_pendingConfigPath = std::string(MSM_PROJECT_ROOT) + "/config/app_pending_config.json";
```

文件顶部 include `"config_handoff.h"`（注意 main_cli 的 CMake 源目录相对包含路径，若为同一目录则 `#include "config_handoff.h"`）。

- [ ] **Step 3: 启动加载改为交接**

替换 `main_hwdaq.cpp:84-90`：

```cpp
    if (ensureRuntimeConfig(g_initConfigPath, g_pendingConfigPath, g_runtimeConfigPath) != 0) {
        std::cerr << "[main] Failed to ensure runtime config\n";
        return 1;
    }
    if (!configModule.config().loadFromJson(g_runtimeConfigPath)) {
        std::cerr << "[main] Failed to load " << g_runtimeConfigPath << "\n";
        return 1;
    }
    std::cout << "[main] Config loaded\n";
```

（删除原有的 `loadFromJson(g_initConfigPath)` 与 `saveToJson(g_runtimeConfigPath)`。）

- [ ] **Step 4: 移除 CLI reload 分支**

删除 `main_hwdaq.cpp:217-232` 的 `if (line == "reload") { ... }` 整个块。

- [ ] **Step 5: 移除 g_reload 分支与 IPC RELOAD**

删除 `main_hwdaq.cpp:242-258` 的 `if (g_reload.exchange(false)) { ... }` 整个块；删除 `g_reload` 定义与 IPC 命令中对 "RELOAD" 的设置（搜索 `g_reload` 与 `"RELOAD"` 全部出现，一并删除）。

- [ ] **Step 6: 编译验证**

Run: `cmake --build main_cli/build 2>&1 | tail -20`（若需要先 `cmake -S main_cli -B main_cli/build`）
Expected: 编译成功，无 "reload"/"g_reload" 残留。

- [ ] **Step 7: 提交**

```bash
git add main_cli/main_hwdaq.cpp
git commit -m "feat(cli): backend-owned pending->runtime handoff at startup; remove reload"
```

---

### Task 3: main_simsrc 接入交接 + 移除 reload

**Files:**
- Modify: `main_cli/main_simsrc.cpp:41-42`（路径常量）
- Modify: `main_cli/main_simsrc.cpp:78-85`（启动加载）
- Modify: `main_cli/main_simsrc.cpp:214-215`（CLI reload）
- Modify: `main_cli/main_simsrc.cpp:243-244`（g_reload）
- Modify: `main_cli/main_simsrc.cpp:25-45`（g_reload 定义与 IPC RELOAD）

**Interfaces:**
- Consumes: `ensureRuntimeConfig(init, pending, runtime)`（Task 1）

- [ ] **Step 1: 加入 pending 路径常量与 include**

同 Task 2 Step 2（路径常量 `g_pendingConfigPath`、`#include "config_handoff.h"`）。

- [ ] **Step 2: 启动加载改为交接**

替换 `main_simsrc.cpp:78-85`：

```cpp
    if (ensureRuntimeConfig(g_initConfigPath, g_pendingConfigPath, g_runtimeConfigPath) != 0) {
        std::cerr << "[main] Failed to ensure runtime config\n";
        return 1;
    }
    if (!configModule.config().loadFromJson(g_runtimeConfigPath)) {
        std::cerr << "[main] Failed to load " << g_runtimeConfigPath << "\n";
        return 1;
    }
    configModule.config().slot = 0; // simsrc: no real hardware
    std::cout << "[main] Config loaded\n";
```

（删除 `saveToJson(g_runtimeConfigPath)`；注意 `slot=0` 保留。）

- [ ] **Step 3: 移除 CLI reload 分支**

删除 `main_simsrc.cpp:214-215` 所在 `if (line == "reload")` 块。

- [ ] **Step 4: 移除 g_reload 分支与 IPC RELOAD**

同 Task 2 Step 5（搜索 `g_reload` 与 `"RELOAD"` 删除）。

- [ ] **Step 5: 编译验证**

Run: `cmake --build main_cli/build 2>&1 | tail -20`
Expected: 编译成功，无 reload/g_reload 残留。

- [ ] **Step 6: 提交**

```bash
git add main_cli/main_simsrc.cpp
git commit -m "feat(cli): simsrc pending->runtime handoff; remove reload"
```

---

### Task 4: webui 编辑目标改为 pending + 首次播种

**Files:**
- Modify: `main_webui/main_hwdaq.cpp:382-385`（路径初始化）
- Modify: `main_webui/main_hwdaq.cpp:528-534`（/api/config 加载源）
- Modify: `main_webui/main_hwdaq.cpp:600-603`（/api/config/reload 移除）
- Modify: `main_webui/main_hwdaq.cpp:384-388`（首次播种）
- Modify: `main_webui/main_hwdaq.cpp:584-592`（saveToInit 移除）

**Interfaces:**
- Consumes: 无新接口（复用 `g_configEditor.importConfig/exportConfig`、`saveToFileAtomic`）。
- Produces: 无对外接口。

- [ ] **Step 1: 读现有初始化段**

Read `main_webui/main_hwdaq.cpp:382-388` 确认 `g_runtimeCfgPath`/`g_initCfgPath` 赋值与首次 importConfig 逻辑。

- [ ] **Step 2: 引入 pending 路径 + 首次播种**

在 `g_runtimeCfgPath`/`g_initCfgPath` 赋值后新增：

```cpp
        g_pendingCfgPath = cand + "app_pending_config.json";
        // 首次运行：无 pending 时用 initial 播种
        {
            std::ifstream pf(g_pendingCfgPath, std::ios::binary);
            bool hasPending = pf.good();
            pf.close();
            if (!hasPending) {
                std::ifstream sf(g_initCfgPath, std::ios::binary);
                std::string content((std::istreambuf_iterator<char>(sf)),
                                    std::istreambuf_iterator<char>());
                if (!content.empty()) {
                    std::string err;
                    if (!g_configEditor.importConfig(g_initCfgPath, &err))
                        std::cerr << "[webui] init seed read failed: " << err << "\n";
                    else if (!g_configEditor.exportConfig(g_pendingCfgPath, &err))
                        std::cerr << "[webui] pending seed write failed: " << err << "\n";
                }
            }
        }
```

同时新增全局 `static std::string g_pendingCfgPath;`（与 `g_runtimeCfgPath` 相邻声明）。

- [ ] **Step 3: /api/config 加载源改 pending**

`main_webui/main_hwdaq.cpp:528` 的 `g_configEditor.importConfig(g_runtimeCfgPath, &err)` 改为 `g_configEditor.importConfig(g_pendingCfgPath, &err)`。

- [ ] **Step 4: 移除 /api/config/reload**

删除 `main_webui/main_hwdaq.cpp:600-603` 整块。

- [ ] **Step 5: 移除 saveToInit 分支**

在 `/api/config/save-app` handler 中删除 `saveToInit` 相关逻辑（`saveToInit` 读取、`applyAppEditToFile(g_initCfgPath)` 调用）。保留 `applyAppEdit` 写 pending 的部分。

- [ ] **Step 6: meta 接口更新**

`/api/config/meta` 中 `runtimeFile` 改为返回 `g_pendingCfgPath`（或同时返回 `pendingFile` 键），并移除 `reload` 相关文案。

- [ ] **Step 7: 编译验证**

Run: `cmake --build main_webui/build 2>&1 | tail -20`
Expected: 编译成功。

- [ ] **Step 8: 提交**

```bash
git add main_webui/main_hwdaq.cpp
git commit -m "feat(webui): edit pending config; first-run seed from initial; remove reload api"
```

---

### Task 5: webui 前端移除 Reload 按钮 + 提示文案

**Files:**
- Modify: `main_webui/web/msm_daq_hw.html:74`（Reload 按钮）
- Modify: `main_webui/web/msm_daq_hw.html:318`（doReload）
- Modify: `main_webui/web/msm_daq_hw.html:498-500`（saveToInit 勾选）
- Modify: `main_webui/web/msm_daq_hw.html:519-520,525-526`（保存提示/reload 调用）

- [ ] **Step 1: 读前端相关行**

Read `main_webui/web/msm_daq_hw.html` 中上述行号附近，确认结构。

- [ ] **Step 2: 移除 Reload 按钮**

删除 `<button class="btn btn-reload" id="btnReload" onclick="doReload()" disabled>↻ Reload</button>` 及其 JS 引用。

- [ ] **Step 3: 移除 doReload 与 reload 调用**

删除 `function doReload()`；删除 `cfgReload()` 或对 `/api/config/reload` 的 fetch 调用（搜 `doReload|config/reload` 全部移除）。

- [ ] **Step 4: 更新保存提示**

将 `toast('已保存，reload 后生效')` 改为 `toast('已保存到待使用配置，下次 Start 生效')`；`save-app` 成功提示同步改为「已保存到待使用配置」。

- [ ] **Step 5: 移除 saveToInit 勾选**

删除 `chkSaveToInit` 复选框及其 JS 读取（`saveToInit` 变量），前端不再双写 initial。

- [ ] **Step 6: 提交**

```bash
git add main_webui/web/msm_daq_hw.html
git commit -m "feat(webui): remove reload button/toast; save-to-pending wording"
```

---

### Task 6: main_config_editor 目标路径改 pending

**Files:**
- Modify: `main_config_editor/main_config_editor.cpp:20-21,93-95,192`（路径解析与保存）

**Interfaces:**
- Consumes: 无。
- Produces: 无。

- [ ] **Step 1: 读现有路径逻辑**

Read `main_config_editor/main_config_editor.cpp:18-100` 确认 `resolveConfigPath` 默认文件名与双文件逻辑。

- [ ] **Step 2: 主操作文件改 pending**

`resolveConfigPath` 默认 `filename = "app_runtime_config.json"` 改为 `"app_pending_config.json"`；删除对 `app_initial_config.json` 的双写/`saveToInit` 逻辑（编辑器只写 pending）。

- [ ] **Step 3: 编译验证**

Run: `cmake --build main_config_editor/build 2>&1 | tail -20`
Expected: 编译成功。

- [ ] **Step 4: 提交**

```bash
git add main_config_editor/main_config_editor.cpp
git commit -m "feat(config_editor): operate on pending config file"
```

---

### Task 7: 全链路测试（simsrc）

**Files:**
- 无源码改动；纯验证。

- [ ] **Step 1: 准备临时目录副本**

在 `/tmp/opencode/cfgtest/` 放置 3 个文件的副本（用 `/tmp` 路径，不污染 repo）：
- 用 `python3` 或 `cp` 复制 `config/app_initial_config.json` 为 init；将 runtime 与 pending 初始删除。

- [ ] **Step 2: 首次运行播种验证**

用修改后的 `msm_cli_simsrc` 直接运行（临时把 `MSM_PROJECT_ROOT` 指向测试目录，或改路径常量跑冒烟），确认启动日志出现 `[config] seeded runtime from init` 且 `app_runtime_config.json` 生成、`app_pending_config.json` 生成。

- [ ] **Step 3: 编辑 pending 生效验证**

修改 pending 的 `frameSize`（如 1152→1280）与一个通道参数；重启 simsrc，确认日志打印新的 `FrameSize`、生效。

- [ ] **Step 4: 无 reload 验证**

确认：重启后进程内不再有 reload 命令/按钮可达路径（CLI 输入 `reload` 无反应；webui 页面无 Reload 按钮）。

- [ ] **Step 5: 提交（无代码变更则不提交）**

如验证中需修正代码，按 Task 2-6 相应修正并单独 commit。

---

## Self-Review 记录

**Spec 覆盖：**
- 文件模型三文件分工 → Task 1-6
- 首次播种 → Task 4
- 运行中编辑不影响后端 → Task 4/5（webui 只写 pending）+ Task 1（后端启动才读 pending）
- 后端自主交接 → Task 1-3
- 无 reload → Task 2/3/4/5
- 原子写 → Task 1 `atomicWrite`、Task 4 复用 exportConfig/applyAppEdit 的原子写
- 交接失败回退 initial → Task 1 `ensureRuntimeConfig` 语义
- simsrc 与 hwdaq 一致 → Task 2/3
- 保留 `auto_acq_on_start` → 不在移除范围（spec 中列为"或保留由你定"，用户未明确要求移除，保持现状）

**占位符扫描：** 无 TODO/TBD；每步含实际代码。

**类型一致性：** `ensureRuntimeConfig(const std::string&, const std::string&, const std::string&)` 在 Task 1 定义，Task 2/3 调用签名一致；`g_pendingCfgPath` 在 Task 4 引入并在 meta/save 处使用一致。

**待办：** Task 4 中 webui 若用 `saveToFileAtomic`（ConfigEditorModule 的 public 方法）需确认其可见性——`exportConfig` 已是 public（Task 4 Step 2 用之），无需额外暴露。
