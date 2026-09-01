---
change: adopt-existing-docs
cabbage_stage: design
change_type: feature
---

# Context

## Current State

- 仓库已有 10 份高质量文档（约 2600 行），但位于非标准位置（`docs/architecture/`、`docs/adr/`、`docs/dev/`、`docs/prd/`、`docs/reviews/`），无站点、无门禁。
- `cabbage init` 已生成标准编号树骨架（00-overview ~ 17-compliance 空目录）、VitePress 配置与 `.github/workflows/cabbage.yml`；站点首页仍是 Cabbage 通用模板。
- `cabbage adopt` 误扫 401 项第三方/构建产物文档（third_party 子模块 193 + aur/src 构建副本 208），需人工剔除（已在 adoption-report.md 记录）。
- 内容脱节：ARCHITECTURE.md 仍为 v3 描述（无 AsrSession/SessionReaper/ResultCoordinator/Mistral），README 双语版缺 Mistral 后端章节。

## Goals and Non-goals

- Goal: 文档迁移到标准树且链接完好；站点定制为项目站点；架构/README/AGENTS 内容同步至当前代码；建立 Cabbage 门禁基线。
- Non-goal: 不改动第三方子模块文档；不重写 ADR 历史内容；不为本变更引入代码功能改动。

# Requirements

| ID | Technical requirement | Source |
|---|---|---|
| TR-1 | 文件移动必须 `git mv`（保留历史），新树路径符合 references/directory-structure.md | R-1 |
| TR-2 | 移动后所有站内/站间 Markdown 链接按新相对路径修复（含反引号文本路径引用） | R-2 |
| TR-3 | VitePress 配置仅保留有真实内容的分区导航；未知分区不建 index | R-3 |
| TR-4 | ARCHITECTURE.md 五处章节（配置/Pipeline/ASR/队列/构建目录/RouteMap/FAQ）以 src/ 头文件为准更新 | R-4 |
| TR-5 | CABBAGE 模板占位符全部替换，无遗留占位标记 | 验证门禁 |

# Design

## Overview

采用「迁移 + 修复 + 定制 + 同步」四步：

1. **迁移映射**（git mv，一批一个逻辑组）：
   - 架构当前态：`ARCHITECTURE.md`、`v4-asr-session-model.md` → `docs/03-architecture/system-design/`
   - 决策历史：ADR → `docs/03-architecture/adr/ADR-0001-gpt-realtime-asr.md`（内容不改写，仅更新失效路径引用）
   - 评审历史：架构评审 → `docs/03-architecture/reviews/`
   - 产品：PRD → `docs/01-product/prd/`；out-of-scope → `docs/01-product/`
   - 开发历史：specs/tasks/research → `docs/06-development/{specs,tasks,research}/`
   - CI 分析：ci 多发行版分析 → `docs/11-ci-cd/`
2. **链接修复**：站内相对链接按新位置重写；ADR/specs/tasks 中的反引号路径同步更新（ADR 属"修复失效引用"而非改写内容）。
3. **站点定制**：为有内容分区写 index.md；首页改为项目 home layout（hero + 特性 + 分区表）；config.ts 只保留有内容分区的 nav/sidebar。
4. **内容同步**：
   - ARCHITECTURE.md：一句话架构/整体架构图（v4 会话模型）、配置表（4 子配置块，以 voiceinput-config.h 为准）、Pipeline（AsrDispatcherLoop/ResultCoordinator/SessionReaper）、ASR（AsrEngine 工厂 + AsrSession + 3 引擎表）、队列（OrderedResultBuffer）、错误处理、构建打包（dlopen、7 发行版矩阵）、目录树、Route Map（勾选已实现项）、FAQ（超时兜底已实现）
   - v4 文档：状态标注「已实现」；迁移路径章节加注完成
   - README 双语：Mistral 后端子配置表 + 描述/功能/使用/注意事项/架构简介更新，架构链接指向新路径
   - AGENTS.md：代码结构树、Pipeline 管道描述、与 ARCHITECTURE.md 关系段更新

## Interfaces and Data

- 站点重写规则（VitePress `rewrites`）已存在：`README.md → index.md`，分区 `:pkg/README.md → :pkg/index.md` 不用改动。
- 链接约定：站内文件间用相对路径；分区 index 到站外（根 README）用 `../../README.md`（仅 VitePress 构建期内解析）。
- 兼容性：ADR 文件标识符从 `2026-08-07-gpt-realtime-asr.md` 改为 `ADR-0001-gpt-realtime-asr.md`（符合命名规范），内容保持不可变。

# Alternatives

| Option | Benefits | Costs and risks | Decision |
|---|---|---|---|
| 仅保留 docs/ 原结构，不迁移 | 零移动成本 | 无法获得编号树、站点路由、门禁；adopt 永远报 migrate | 否决 |
| 把根 README/CHANGELOG 也迁入 docs/ | 站点内全量一致 | 违反 GitHub 开源惯例（根 README 必须存在），需维护镜像 | 否决（保留根文件 + 00-overview 概览） |
| 用 `cp` 而非 `git mv` | 避免移动导致链接改动 | 失去历史追溯，产生双份内容违反单一事实源 | 否决 |
| 同步 ARCHITECTURE.md 而非整体并入 v4 提案 | 保持当前态/历史分离 | — | 采纳（v4 文档保留为详细设计，状态标注已实现） |

# Testing Decisions

- 本变更为文档/结构变更，无单元测试源（`tests/` 为空目录）。验证以 `cabbage verify/validate` + `cabbage docs build` + 源码交叉核对为准（见 test-plan.md）。
- 代码行为不变；无需 TDD RED/GREEN 循环。任何源码改动不在本变更范围。

# Failure Modes

| 失败模式 | 影响 | 处置 |
|---|---|---|
| 迁移后链接失效（人为遗漏） | validate 失败 | 本变更 T-1 全量 `rg` 扫描 + `cabbage validate` 拦截 |
| VitePress 构建失败（语法/链接） | CI 门禁失败 | `cabbage docs build` 本地先行；失败按报错路径修复 |
| cabbage.yml 引用的文件不存在 | CI job 失败 | T-4 核验 requirements.txt / cabbage_cli / tests，缺失则改用仓库内可执行命令 |
| ARCHITECTURE 与代码再次脱节 | 文档误导 | 后续变更须按 Cabbage 门禁同步当前状态文档（require_current_state_docs） |

# Rollout

- 单 PR 交付（Atomic PR Delivery）：迁移 + 站点 + 内容同步 + CI 修正一次合并。
- 合并后由 cabbage.yml 对 main push 部署 GitHub Pages（若 Pages 未启用则裁剪部署 job，保留 validate）。
- 回滚：`git revert` 整个 PR；文档为纯文本资产，无数据迁移。

- 本变更为文档/结构变更，无单元测试源（`tests/` 为空目录）。验证以 `cabbage verify/validate` + `cabbage docs build` + 源码交叉核对为准（见 test-plan.md）。
- 代码行为不变；无需 TDD RED/GREEN 循环。任何源码改动不在本变更范围。