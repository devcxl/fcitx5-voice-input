---
change: tidy-docs-tree
cabbage_stage: impact
change_type: refactor
---

# Change Summary

对 `docs/` 文档树做一次纯文档结构整理与内容纠偏：清理 `cabbage sync` 遗留在长生命周期分区里的 3 份变更工作区产物（`adopt-existing-docs.md`，归档区已有完整副本）；修正 ARCHITECTURE.md 与双语 README 中与代码脱节的 4 处描述（幽灵 `config.h`、已移除的 `AudioSource` 配置项、typo、模糊 PR 引用）；并在 `.cabbage/config.yaml` 的 `docs.mapping` 中关闭 tech-spec / test-plan / release-plan 向 `docs/` 的自动同步，消除"每次变更都向当前态文档树注入变更产物"的结构性病根。受影响对象：文档读者与维护者、后续 Cabbage 变更流程；代码零改动。

# Impact Matrix

| Area | Impact | Notes |
|---|---|---|
| Product | Yes | `docs/01-product/` 移除 1 份不属于产品需求的变更产物文件，分区回归纯产品文档 |
| Architecture | No | 代码架构不变；ARCHITECTURE.md 仅做与代码一致性的文字纠偏 |
| API | No | |
| Database | No | |
| Security | No | |
| Testing | Yes | `docs/08-testing/` 移除变更产物文件；该目录回归空置（项目当前无测试文档）；验证以门禁命令为准 |
| Deployment | No | |
| Operations | No | |
| Data | No | |
| Performance | No | |

# Impact Details

- **Product**: 删除 `docs/01-product/adopt-existing-docs.md`（adoption 变更的 PRD 产物，内容完整保留于 `.cabbage/archive/2026/adopt-existing-docs/prd.md`，且站点导航从未收录该文件）。`docs/01-product/` 回归"PRD + Out of Scope"的纯产品语义。
- **Testing**: 删除 `docs/08-testing/adopt-existing-docs.md`（adoption 变更的 test-plan 产物）。项目无测试代码，该分区本应为空；同步映射关闭后未来变更不会再向此目录注入文件。
- **结构性修正**: `.cabbage/config.yaml` 新增 `docs.mapping` 覆盖：`design`/`tests`/`release` 置空（跳过同步），`requirement` 精确到 `01-product/prd/` 子目录。依据：skill 规范 lifecycle.md 的 sync 目标仅含 api-design / database-design / adr / rfc；tooling 默认映射把 tech-spec / test-plan 也灌入 `docs/`，正是上一变更遗留 3 份孤儿文件的根因。
- **内容纠偏**: ARCHITECTURE.md 修正 `src/addon/config/` 目录树（删除不存在的 `config.h` 桥接头）、`AudioSource` 配置项描述（89397a7 已移除音频源选择，现为自动选源）、"flow/非流式"typo、模糊的"过往 PR"引用（补 PR #20）；双语 README 修正已失效的 `AudioSource` 下拉框指引。

# Risks

| Risk | Likelihood | Impact | Mitigation | Owner |
|---|---|---|---|---|
| 删除的 3 份同步产物仍被外部（Issue/PR 评论）或站内链接引用 | Low | 死链、读者困惑 | 全仓 grep 确认无站内入链；git 历史与 `.cabbage/archive/` 保留完整内容可追溯；`cabbage validate` 拦截站内死链 | maintainer |
| `docs.mapping` 覆盖影响后续变更的 sync 行为 | Low | 后续变更工件不再自动出现在 `docs/` | 该行为正是本变更目标（变更历史归 `.cabbage/`，`docs/` 只放当前态文档）；adr/rfc/api 等规范允许的映射保持不变 | maintainer |
| ARCHITECTURE.md 文字纠偏引入新的与代码不一致描述 | Low | 文档误导 | 修改以源码实读为准（`voiceinput-config.h`、`pulse_audio_capture.cpp`），test-plan T-4 复核 | maintainer |

# Documentation Updates

- 删除：`docs/01-product/adopt-existing-docs.md`、`docs/03-architecture/system-design/adopt-existing-docs.md`、`docs/08-testing/adopt-existing-docs.md`
- 修正：`docs/03-architecture/system-design/ARCHITECTURE.md`（4 处）、`README.md`（1 处）、`README.zh-CN.md`（1 处）
- 配置：`.cabbage/config.yaml`（新增 `docs.mapping` 覆盖）
