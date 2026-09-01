---
change: tidy-docs-tree
cabbage_stage: design
change_type: refactor
---

# Context

## Current State

- `docs/` 树（Cabbage 编号分区）中残留 3 份上一变更 `adopt-existing-docs` 的同步产物：`docs/01-product/adopt-existing-docs.md`（PRD）、`docs/03-architecture/system-design/adopt-existing-docs.md`（tech-spec）、`docs/08-testing/adopt-existing-docs.md`（test-plan）。它们是变更工作区工件而非当前态文档，站点导航（`docs/.vitepress/config.ts`）与分区索引均未收录，属孤儿页面；完整副本已存在于 `.cabbage/archive/2026/adopt-existing-docs/`。
- 根因：tooling 的 `DEFAULT_STAGE_DOCS_MAPPING` 把 `design`/`tests`/`release` 阶段工件也映射进 `docs/`（`core.py`），超出 skill 规范 lifecycle.md 约定的同步目标（仅 api-design / database-design / adr / rfc）。`cabbage sync` / `archive` 每次都会重复注入，不治本则后续变更继续污染。
- 文档与代码存在 4 处脱节（均经源码核实）：
  1. `ARCHITECTURE.md` 配置章节的目录树列出 `src/addon/config/config.h`（桥接头文件），实际目录仅有 `voiceinput-config.h`；
  2. `ARCHITECTURE.md` 与双语 README 描述 `AudioSource` 配置键/下拉框，但 commit 89397a7（"移除音频源选择"）已删除该配置项；现行为为 `pulse_audio_capture.cpp` 中 `FindBestSourceName()` 经 `pactl list sources short` 自动选源（优先 `alsa_input.*`，排除 `.monitor` / `echoCancel`）；
  3. `ARCHITECTURE.md` Route Map 中"LLM 后处理（flow/非流式）"为笔误，应为"流式/非流式"；
  4. `ARCHITECTURE.md` 依赖章节"详见过往 PR：Arch 构建 `-z,now` 兼容"引用模糊，该 PR 为 #20（AGENTS.md 有明确记录）。

## Goals and Non-goals

- Goal: `docs/` 各分区只保留当前态/历史决策文档，无变更产物残留；文档描述与代码一致；后续变更的 tech-spec / test-plan / release-plan 不再被自动灌入 `docs/`。
- Non-goal: 不改动任何 C++ 源码与构建脚本；不迁移/重命名 ADR、评审、调研等历史记录（不可改写）；不重排 `docs/06-development/`（已有索引说明，属合规的历史产物区）；不改 VitePress 导航结构（删除的文件本就不在导航内）。

# Requirements

| ID | Technical requirement | Source |
|---|---|---|
| TR-1 | 3 份 `adopt-existing-docs.md` 同步产物 MUST 从 `docs/` 移除（git rm，保留归档副本与 git 历史） | Impact: Product/Testing；单一事实源原则 |
| TR-2 | `.cabbage/config.yaml` MUST 新增 `docs.mapping` 覆盖：`design`、`tests`、`release` 置空跳过同步，`requirement` 精确到 `docs/01-product/prd/`；adr/rfc/api/database/security 映射保持 tooling 默认 | lifecycle.md 同步目标约定；风险"mapping 影响后续变更" |
| TR-3 | `ARCHITECTURE.md` 的配置目录树、`AudioSource` 描述、Route Map typo、PR 引用 MUST 修正为与 `src/addon/` 现状一致 | T-4 源码核对 |
| TR-4 | 双语 README 的"音频设备/Audio Device"条目 MUST 改写为自动选源现状（不再引导用户使用不存在的下拉框） | T-5 |
| TR-5 | 全部修改后 `cabbage validate --all` 与 `cabbage docs build` MUST 通过（零死链、零占位符） | 验证门禁 |

# Design

## Overview

三类改动，互相独立：

1. **删除同步产物（TR-1）**：`git rm` 三份文件。`docs/08-testing/` 回归空置（与 `02-design` 等未使用标准分区一致，git 不跟踪空目录）。
2. **同步映射治本（TR-2）**：`core.py:stage_docs_mapping` 读 `config.yaml` 的 `docs.mapping` 合并覆盖默认映射，映射值为空字符串时 `sync_change_to_docs` 直接跳过该阶段。新增配置：
   ```yaml
   docs:
     dir: docs
     mapping:
       requirement: '01-product/prd/{change_id}.md'
       design: ''
       tests: ''
       release: ''
   ```
   效果：变更的 tech-spec / test-plan / release-plan 留在 `.cabbage/changes|archive/`（工作区历史），`docs/` 只接收 adr / rfc / api-design / database-design / security-review 等明确应当长期保留的设计文档；PRD 若未来出现则归档到 `01-product/prd/` 子目录，与既有 `prd/add-input-method-icon.md` 布局一致。
3. **内容纠偏（TR-3/TR-4）**：按上文列出的 4+2 处逐点改写，措辞以源码实际行为为准。

## Interfaces and Data

- 变更涉及面：纯 Markdown 与 `.cabbage/config.yaml`，无代码接口、数据流、状态变化。
- `docs.mapping` 是 `.cabbage/config.yaml` 的新增子键，属于对该文件的向后兼容扩展（缺失时 tooling 回落默认映射），不影响 CI 中 `cabbage_cli` 其余检查逻辑。
- 站点层面：被删文件无入链（已全仓 grep 验证），VitePress 导航/搜索无需改动。

# Alternatives

| Option | Benefits | Costs and risks | Decision |
|---|---|---|---|
| 仅删除 3 份文件，不改 `docs.mapping` | 改动最小 | 根因保留：下一次 `cabbage sync` 又注入同类产物，问题复发 | 否决 |
| 修改 tooling `DEFAULT_STAGE_DOCS_MAPPING` 源码 | 一步到位 | `tooling/` 是 vendored CLI，升级/init 会被覆盖；且属代码改动，超出文档整理范围 | 否决 |
| `docs.mapping` 覆盖（本设计） | 配置层治本、vendored 代码零改动、升级安全；映射值留空即跳过是 `stage_docs_mapping` 明确支持的语义 | config.yaml 变更需人工评审（CODEOWNERS 已覆盖） | 采纳 |
| 把 3 份产物移入 `docs/archive/` 而非删除 | 站内仍可检索 | 与 `.cabbage/archive/` 双份冗余，违反单一事实源；且"变更产物"不应占据当前态树 | 否决 |
| README 的 AudioSource 段保留并"注明已废弃" | 不删内容 | 代码中该功能已彻底移除，无废弃路径可指，保留只会误导 | 否决 |

# Security and Privacy

N/A — 不触及任何认证、密钥、数据面；删除的文件不含敏感信息（均为流程性文档）。

# Observability

| Signal | Purpose | Alert or dashboard |
|---|---|---|
| `cabbage validate --all` 退出码 | 证明 `docs/` 无死链、无占位符 | 本地/CI 门禁输出 |
| `cabbage docs build` 退出码 | 证明 VitePress 站点可构建、无死链 | 本地/CI 门禁输出 |
| `git status` 干净度 | 证明改动范围仅限声明文件 | 汇总时人工核对 |

# Failure Modes

| Failure mode | Detection | Handling | Recovery |
|---|---|---|---|
| 删除文件导致未知站内死链 | `cabbage validate` 报 broken link | 按 validate 输出的引用位置修正或补充必要链接 | git revert 单个文件删除 |
| `docs.mapping` YAML 语法错误 | `cabbage` 任意命令报 invalid YAML | 修正 YAML 后重跑 | 还原 config.yaml 该段 |
| 纠偏后的描述仍与代码有出入 | test-plan T-4 复核发现 | 以源码为准再次改写 | 保留原文表述并在 ARCHITECTURE.md 待办中登记 |
| verify 因模板占位符/未勾选任务失败 | `cabbage verify` 报错清单 | 按报错逐条清理后重新 verify | N/A（纯文本迭代） |

# Rollout

- 单 PR 交付（Atomic PR Delivery）：删除 + 配置 + 纠偏同 PR；无功能开关、无兼容窗口。
- 成功标准：TR-1~TR-5 全部满足，`cabbage gate tidy-docs-tree merge` 通过，CI（`cabbage ci --base origin/main` + 文档构建 job）绿。

# Rollback

- 触发：合并后发现站点构建失败或纠偏内容有误。
- 步骤：`git revert` 该 PR（纯文本/配置资产，无数据迁移、无缓存失效问题）。
- 约束：若仅纠偏内容有误而结构部分正确，可部分 revert 对应文件，不影响其余改动。

# Open Questions

- N/A（无未决技术决策；mapping 覆盖语义已从 `core.py` 源码确认）。
