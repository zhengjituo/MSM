# Task 003 — 目录重构 + Workflow 落实

## 起始 commit
```
2f7aa5d backup: frame architecture restructuring
```

## TODO
- [x] 创建 task 文件
- [ ] 创建新目录结构
- [ ] 搬移文件到新位置，更新 CMakeLists.txt
- [ ] 重写根 AGENTS.md 为顶层入口 + workflow 约束
- [ ] 更新根 CMakeLists.txt 为聚合模式
- [ ] 编写各模块 docs/*.md 设计说明
- [x] 编译测试通过（sim 模式 300K 帧跨通道对齐确认）
- [ ] git commit 备份

## 日志

### 目录设计决议
- 旧残留保留不动，不纳入新结构
- CMake: 混合模式 — 根 CMakeLists.txt 一键构建 + 各 main_v* 可独立构建
- 模块扁平化: module_xxx/，共享依赖在顶层（config/、extern/、sdk_include/）
- docs/ 下每模块同名 .md 设计说明
- tasks/ 下流水号 .md，包含起始 commit、日志、结束 commit

## Done

- 目录结构按决议落地
- AGENTS.md 作为顶层入口
- 构建通过

## 结束 commit
<!-- 完成后填写 -->
