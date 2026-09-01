---
change: adopt-existing-docs
cabbage_stage: tests
change_type: feature
---

# Strategy

本变更为文档/结构变更：代码零改动，无单元测试源（tests/ 为空目录）。验证以自动化门禁 + 源码交叉核对为主。

| Level | Scope | Test seam | Owner |
|---|---|---|---|
| 静态门禁 | 全部 docs/*.md、.cabbage/changes | `cabbage verify / validate`（占位符、坏链、工件完整性） | maintainer |
| 站点构建 | docs/ 全量 | `cabbage docs build`（VitePress 构建 + 死链检查） | maintainer |
| 内容一致性 | ARCHITECTURE.md 组件章节 | 对照 src/addon/ 头文件逐节核对 | maintainer |
| 引用完整性 | 迁移后文档 | `rg` 扫描旧路径残留 | maintainer |

# Test Environment and Data

- 本机：cabbage 已 init，pnpm 可用（docs/pnpm-lock.yaml），Python 3.14 + pyyaml。
- 无需外部账号/网络（站点构建仅依赖本地 pnpm 依赖）。
- 源码现状快照：v0.5.0（Mistral Realtime 已合入），作为 ARCHITECTURE 核对基准。

# Cases

| ID | Scenario | Level | Expected result | Priority |
|---|---|---|---|---|
| T-1 | 迁移后无残留旧路径引用（docs/architecture、docs/dev、docs/adr、docs/prd、docs/reviews） | 静态门禁 | `rg` 扫描返回空 | High |
| T-2 | docs 构建 | 站点构建 | VitePress build 成功，无死链 | High |
| T-3 | validate 全量 | 静态门禁 | 无 broken link / 无 CABBAGE 占位符 / 无遗留占位标记 | High |
| T-4 | ARCHITECTURE 与代码核对 | 内容一致性 | 配置键（4 块）与 voiceinput-config.h 一致；Pipeline/ASR 与 pipeline.h/asr_*.h 一致 | High |
| T-5 | ADR 历史完整性 | 内容一致性 | ADR-0001 内容与迁移前一致（仅链接修复） | Medium |

# Regression Coverage

- 既有 CI（ci.yml 7 发行版矩阵 / release.yml）不受影响：本变更不触碰 CMake 与构建脚本。
- cabbage.yml 为新增 workflow；验证其可执行性（requirements.txt / cabbage_cli / tests 引用）并在本 PR 内修正。
- docs 站点构建失败会在 CI 的 cabbage.yml job 中拦截，防止后续回归。