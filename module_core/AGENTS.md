# Module Core - 框架核心模块

## 模块概述

消息驱动状态机框架核心组件，提供 Message、MessageQueue、Module 基类等基础设施。

## 目录结构

```
module_core/
├── AGENTS.md           # 本模块规范
├── include/            # 头文件
├── src/                # 源文件
├── build/              # 构建输出
├── tests/              # 单元测试（可选）
└── CMakeLists.txt      # 构建配置
```

## 设计原则

复用 `V:\share\vs\vscode\ThreadStateMachine` 的基础架构，扩展消息格式为 JSON。

## 开发任务

1. Message 结构（cmd + data）
2. MessageQueue 实现
3. Module 基类
4. NI 字符串解析器

## 依赖

- nlohmann/json（JSON 处理）
- ThreadStateMachine（线程管理）

## Task 历史

（待补充）