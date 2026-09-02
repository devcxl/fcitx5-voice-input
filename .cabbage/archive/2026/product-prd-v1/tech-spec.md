---
change: product-prd-v1
cabbage_stage: design
change_type: feature
---

# Context

## Current State

产品仓库已有功能级 PRD（`docs/01-product/prd/add-input-method-icon.md`，针对"输入法图标"单点功能），但没有覆盖完整产品形态的现状基线文档。`src/addon/`（v0.5.0）已实现音频捕获（PulseAudio/PipeWire 双后端 + dlopen）、Silero VAD 分段、ASR 多后端（OpenAI 兼容 whisper/chat/realtime、Volcengine、Mistral）、LLM 后处理、结果保序/会话管理、Fcitx5 集成 UI、安全与打包等能力，却缺乏一份从实现反推的、可测试的产品需求文档。

本变更以纯文档方式反推并固化该基线，不改任何源码。

## Goals and Non-goals

- Goal: 产出一份从当前代码反推的产品级 PRD（现状还原型），含可测试需求与验收场景，纳入 Cabbage 生命周期并同步到正式文档树。
- Non-goal: 不引入新功能需求、不修改源码、不制定 roadmap 交付计划、不为未实现功能（本地 ASR/Command/场景/热词）编写需求。

# Requirements

| ID | Technical requirement | Source |
|---|---|---|
| TR-1 | 反推范围严格限定于 `src/addon/` 当前已实现行为，不得外推未实现功能 | prd.md（现状还原定位） |
| TR-2 | PRD 含 Goal、Scope（In/Out）、可测试 Requirements（SHALL/MUST）、Acceptance Criteria、Success Metrics、Risks | feature 工作流 required_headings |
| TR-3 | 未实现的前瞻能力（本地 ASR/Command/场景/热词）在 Out of Scope 显式标注 | 用户决策（现状还原型） |
| TR-4 | 产物经 `cabbage verify/sync/validate` 校验，并同步至 `docs/01-product/prd/product-prd-v1.md` | cabbage 生命周期 |
| TR-5 | 与既有 `add-input-method-icon.md` 范围不重叠，后者作为历史单点 PRD 保留 | impact.md 文档更新计划 |

# Design

## Overview

本变更是一个"代码 → 文档"的现状还原流程，非软件系统设计：

1. **源事实核实**：审读 `src/addon/` 关键模块（`engine.cpp/h`、`pipeline.cpp`、`vad.cpp`、`asr_engine.h`、`asr_session.h`、`voiceinput-config.h`、`types.h`、`openai_asr.cpp`、`CMakeLists.txt`、`voiceinput.conf.in`）与 `README.md`、历史 PRD，提取已实现的能力、配置项、默认值与约束。
2. **规范化**：按 feature 工作流 `prd.md` 模板组织为 Goal / Users / Scope / Requirements / Acceptance Criteria / Success Metrics / Risks，需求以可测试的 SHALL/MUST 措辞书写。
3. **对齐**：`cabbage impact` 仅启用 product/test 影响；后续阶段（design/tests/implementation）按文档任务最小化填写真实内容，不制造空洞实现工件。

数据流与状态机为既有系统行为，本变更不改动，故不新增实现时序图（N/A）。

## Interfaces and Data

- **文档接口**：新增 `docs/01-product/prd/product-prd-v1.md`（经 `cabbage sync` 由审计后的 prd.md 生成，frontmatter 含 origin_change/change_type/cabbage_stage/synced_at）。
- **导航接口**：`.vitepress/config.ts` sidebar 与 `docs/README.md` 目录表格挂载新页面，无路由冲突。
- **数据契约**：24 条需求（R-1~R-24）与 6 个验收场景直接映射代码事实；PRD 不持有运行期数据。
- **兼容约束**：不得破坏既有 `add-input-method-icon.md` 与 `out-of-scope.md` 链接与结构。

# Alternatives

| Option | Benefits | Costs and risks | Decision |
|---|---|---|---|
| 现状还原型 PRD（选定） | 文档即代码事实，可测试、与实现严格一致 | 代码演进后需同步更新 | 用户决策采用 |
| 目标/愿景型 PRD | 可纳入 roadmap 前瞻 | 与现状脱节，验收不可落地 | 否决（用户明确选择现状还原） |
| 不新增 PRD，仅扩充现有功能级 PRD | 文件最少 | 无法覆盖多能力域，结构不适配 | 否决（范围不匹配） |

# Security and Privacy

N/A，理由：本变更为纯 Markdown 文档交付，不引入运行时代码、不处理凭据、不接触音频数据。PRD 文本引用 API Key 配置项时仅作配置项描述，不含实际凭据值。

# Observability

| Signal | Purpose | Alert or dashboard |
|---|---|---|
| `cabbage verify/validate` 通过输出 | 证明产物满足生命周期门禁 | Cabbage CI（cabbage.yml） |
| `docs pnpm build` 成功 | 证明导航链接有效、站点可部署 | Cabbage CI / Pages 部署 |

# Failure Modes

| Failure mode | Detection | Handling | Recovery |
|---|---|---|---|
| PRD 与代码事实脱节（反推遗漏/过时） | 审阅对照 `src/addon/` 发现行为不符 | 定位脱节条目并更正 PRD | 更新 PRD 后重跑 verify/validate |
| 文档链接失效（导航/内链） | `cabbage validate` 报告 broken link | 修正链接目标 | 重跑 validate + docs build |
| 与既有 PRD 内容冲突 | 双重事实（多源不一致） | 明确范围归属（总览 vs 单点） | 更新冲突方并同步 |

# Rollout

N/A，理由：纯文档变更无需部署窗口/功能开关；随变更提交合并即为生效，产物经 `cabbage sync` 落盘。

# Rollback

N/A，理由：纯文档无数据迁移；回滚即 `git revert` 删除新增 PRD 与导航改动，无数据安全约束。

# Open Questions

- N/A — 本变更为文档现状还原，无未决技术决策；roadmap 功能已在 Out of Scope 标注，待未来单独立项。