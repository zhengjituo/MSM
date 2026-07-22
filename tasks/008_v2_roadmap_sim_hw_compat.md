# Task 008 — 修订 V2/V3 路线图 + main_simsrc 对齐 hwdaq

## 起始 commit
```
59986df docs: fill end commit and tag in task 007
```

## TODO
- [x] AGENTS.md: V2=Storage保存文件, V3=WebUI, 修正 V3 非 TUI
- [x] main_simsrc.cpp: 去掉自动 start，加 start/stop 命令，IPC 接口与 hwdaq 一致
- [x] WebUI 从 V2 备份到 V3
- [x] simsource 注册名为 DAQModule，IPC 可控制
- [x] msm_ipc_shell 发送裸命令 start/stop/status/reload
- [x] SIGPIPE 处理
- [x] 编译验证 + 实测通过
- [x] git commit

## 日志
- V1 CLI 已稳定，V3 目标改回 WebUI（不是 TUI），V2 目标改为加入 storage 保存文件功能
- main_simsrc 改造为与 hwdaq 同样的用户驱动模式，方便无硬件测试
- V3（原 TUI 原型）已清空，从 V2 备份 WebUI 代码作为起点：
  - `module_main_v3/main_hwdaq.cpp` （来自 V2）
  - `module_main_v3/web/msm_daq_hw.html` + `status.html`
  - CMakeLists.txt 改为构建 `msm_web_v3`
- 删除 V3 旧文件：`main_tui.cpp`、`main.cpp`
- 根 CMakeLists.txt 移除 module_tui 引用
- simsource 模块名从 `SimDAQ` 改为 `DAQModule`，IPC shell 的 `start/stop/status` 可正常控制
- `msm_ipc_shell` 的 start/stop/status/reload 改为发送裸命令（不带 `MSG` 前缀），让 IPCServer 的 START/STOP 处理器正确处理 Process + DAQModule 双模块
- 验证通过：`msm_cli_simsrc` + `msm_ipc_shell` 全链路 start → status → stop → clean exit

## 结束 commit
```
c1080ee fix: simsource as DAQModule, IPC shell bare commands, SIGPIPE handling
```

## Tag
`v2-roadmap`