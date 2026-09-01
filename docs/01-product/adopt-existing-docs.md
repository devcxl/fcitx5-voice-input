---
origin_change: adopt-existing-docs
change_type: feature
cabbage_stage: requirement
synced_at: '2026-09-01T21:13:10.934548+00:00'
---

# Goal

将 fcitx5-voice-input 既有的 10 份长生命周期文档纳入 Cabbage 标准文档树（`docs/00-` ~ `docs/17-`），建立文档生命周期门禁（PRD → 设计 → 验证 → 归档），并消除文档与当前代码（v4 会话模型、Mistral/Realtime 引擎）的脱节。产出可构建、链接可校验的项目文档站（VitePress）。

# Users and Use Cases

| User or actor | Need | Primary use case |
|---|---|---|
| 开发者 / 维护者 | 文档结构可预测、路径稳定 | 按编号分区定位架构、PRD、ADR、CI 文档，链接永不失效 |
| 新贡献者 | 快速理解当前架构，不受历史文档误导 | 阅读 ARCHITECTURE.md 获得与 src/addon/ 一致的现状描述 |
| CI 系统 | 文档变更可自动验证、拒绝占位符与坏链 | 每次 PR 运行 cabbage validate / docs build 门禁 |

# Scope

## In Scope

- 迁移 docs/ 下 10 份历史文档到 Cabbage 标准编号树（git mv 保留历史）
- 修复迁移后全部相对链接与交叉引用
- 定制 VitePress 站点（项目首页 + 分区索引 + 导航/侧栏）
- 内容同步：ARCHITECTURE.md 更新至 v4 会话模型与全部 ASR 引擎；README 双语版补充 Mistral 后端；v4 设计文档状态改为已实现；AGENTS.md 目录结构/文档引用同步
- 建立 adoption 基线变更记录（本变更），后续所有文档改动受门禁约束

## Out of Scope

- `third_party/`（git submodule 上游文档）与 `aur/src/`（makepkg 构建产物副本）不迁移、不收录
- 新增产品功能、改写历史 ADR 内容（ADR 仅移动位置、修复失效引用）
- 根目录 README / CHANGELOG / AGENTS.md 保留原位置（开源惯例），仅更新内容与链接

# Requirements

| ID | Requirement (SHALL/MUST) | Priority | Rationale |
|---|---|---|---|
| R-1 | 全部 10 份既有文档 MUST 迁移至 `docs/` 标准编号树，且通过 `git mv` 保留文件历史 | Must | 可追溯性与稳定路径 |
| R-2 | 迁移后文档内相对链接 MUST 全部可解析（validate 无 broken link） | Must | 严格链接完整性 |
| R-3 | VitePress 站点 MUST 构建成功（`cabbage docs build`）并包含项目自有首页 | Must | 站点可用且非模板占位 |
| R-4 | ARCHITECTURE.md MUST 与 `src/addon/` 当前实现一致（v4 会话模型、4 个 ASR 引擎、ResultCoordinator/SessionReaper） | Must | 单一事实源防误导 |
| R-5 | adoption 决策（迁移映射、排除项、保留项）MUST 记录在 `.cabbage/adoption-report.md` | Must | 决策可审计 |

# Acceptance Criteria

### Scenario 1: 站点可构建且链接完整
- **GIVEN**: 变更完成的 docs/ 树
- **WHEN**: 执行 `cabbage validate` 与 `cabbage docs build`
- **THEN**: 无 broken link、无未验证工件，站点构建产物生成
- [x] validate 通过、docs build 成功

### Scenario 2: 架构文档与代码一致
- **GIVEN**: 当前 src/addon/ 代码（v4 会话模型）
- **WHEN**: 对照 ARCHITECTURE.md 组件章节
- **THEN**: Pipeline/ASR/队列/配置/目录结构描述与代码一致，无 v3 遗留描述
- [x] ARCHITECTURE.md 已同步 v4 并交叉核对源码

### Scenario 3: 历史文档完整保留
- **GIVEN**: 迁移前的 docs/ 内容
- **WHEN**: 检查新树与 git 历史
- **THEN**: 10 份文档全部存在且内容完整（ADR 未改写）
- [x] 迁移后文件清单与迁移计划一致

# Success Metrics

| Metric | Baseline | Target | Measurement window |
|---|---|---|---|
| docs 站点构建 | init 模板（不可用） | `cabbage docs build` 100% 通过 | 本变更 merge 时 |
| 失效链接数 | 迁移后必然存在 | 0 | `cabbage validate` 
| ARCHITECTURE.md 与代码脱节章节 | 5+（Pipeline/ASR/队列/配置/目录/RouteMap/FAQ） | 0 | merge 后抽查 |

# Dependencies and Constraints

- 需 `git submodule update --init --recursive` 的仓库结构（third_party 文档不迁移）
- cabbage CLI 环境（python + pyyaml + git + pnpm）已通过 `cabbage doctor`
- 不修改 CI 现有 ci.yml / release.yml 行为；仅新增 cabbage.yml 文档门禁

# Risks

| Risk | Impact | Mitigation |
|---|---|---|
| cabbage.yml 引用 `requirements.txt` / `python -m cabbage_cli` / `tests/` 在仓库中不存在，CI 失败 | 高 | 验证并修正 workflow，缺失部分裁剪为可执行步骤 |
| 迁移后旧链接被外部引用（Issue/PR 评论） | 低 | 文档树为仓库内资产，git mv 留有完整历史 |
| 内容同步引入与代码细节不一致 | 中 | 以源码头文件为准交叉核对（本次已完成核对记录于 tech-spec） |

# Open Questions

- N/A（无未决决策；cabbage.yml CI 可用性在任务 T-4 验证后调整）