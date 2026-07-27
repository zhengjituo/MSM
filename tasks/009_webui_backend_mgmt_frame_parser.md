# Task 009 — WebUI 后端进程管理 + C++ frame_parser 库

## 起始 commit
```
58dfcd9 task: update 008 with completion status
```

## TODO
- [x] C++ frame_parser 库（frame_parser.h/cpp），按 frame_format_PD.md 正确解析 296B 帧
- [x] test_parse 独立测试程序，对比 Python 输出 1:1
- [x] Python GUI 工具（ZkcxAnalysis_20260727.py）解析修正 + Smart Open
- [x] module_process 集成 frame_parser：BatchFrameData 扩展、extractFrame 重写、自动 100ms 对齐
- [x] webUI 后端进程管理：Run（fork+exec）+ Quit（SIGTERM/KILL）
- [x] webUI 状态机：launching（等 IPC）→ launching（等 DAQ ready）→ running
- [x] webUI 模式选择：HW/SIM
- [x] webUI 特征表 SSE 推送 + 前端表格显示
- [x] webUI 帧索引输入框
- [x] webUI 保存/DAQ 状态指示灯动态标签（Run/Stop, On/Off）
- [x] 编译验证全部通过

## Session Git Log

### 2026-07-27  frame_parser + webUI backend mgmt
```
<to be filled after commit>
```

## 日志

### 设计决议
- frame_parser 独立于 module_process，可供 Python 通过 ctypes 调用或单独测试
- 解析偏移量以参考 C++ 实现（data_save_parameter_define.cpp:95-132）为准，Python 参考工具有 4 处偏移错误
- webUI 通过 fork+exec 管理后端进程，用 waitpid(WNOHANG) 监控子进程生死
- backend 状态在 DAQModule 返回含 `running` 字段的响应后才从 launching 切到 running

### 已修正
- frame_parser 4 处偏移/算法 bug 与 Python 参考工具对齐
- webUI 后端状态 timing：IPC 连接后等 DAQ 模块就绪才亮 running

## 结束 commit
```
<to be filled after commit>
```
