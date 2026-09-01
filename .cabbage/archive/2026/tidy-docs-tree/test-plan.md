---
change: tidy-docs-tree
cabbage_stage: tests
change_type: refactor
---

# Strategy

本变更为纯文档/配置整理：C++ 源码与构建脚本零改动（`tests/` 本身为空目录），无单元测试源。验证策略是"静态门禁 + 内容核对 + 站点构建"三层：结构类改动（删文件、mapping）靠门禁命令与全仓扫描证明；内容类改动（ARCHITECTURE.md / README 纠偏）靠对照源码逐条复核证明。

| Level | Scope | Test seam | Owner |
|---|---|---|---|
| 静态门禁 | `.cabbage/changes/tidy-docs-tree/` 全部工件 | `cabbage verify`（占位符、必需标题、未勾选任务、链接、Mermaid） | maintainer |
| 全仓扫描 | `docs/`、`README.md`、`README.zh-CN.md` | `git status` 变更清单；`grep` 扫描被删文件名与 `AudioSource`/`config.h` 残留引用 | maintainer |
| 站点构建 | `docs/` 全量 | `cabbage docs build`（VitePress 构建含死链检查） | maintainer |
| 内容一致性 | ARCHITECTURE.md 纠偏处 | 对照 `src/addon/config/voiceinput-config.h`、`src/addon/capture/pulse_audio_capture.cpp`、AGENTS.md 逐条核对 | maintainer |

# Test Environment and Data

- 本机：cabbage CLI 可用（`cabbage doctor` 通过），pnpm 依赖已安装（`docs/node_modules/` 存在）。
- 无需外部服务、账号或网络（站点构建仅依赖本地依赖）。
- 核对基准：`main` 分支源码（89397a7 已移除音频源选择；AGENTS.md 记录 PR #20 为 `-z,now` 兼容 PR）。

# Cases

| ID | Scenario | Level | Expected result | Priority |
|---|---|---|---|---|
| T-1 | 3 份 `adopt-existing-docs.md` 从 `docs/` 移除后无站内入链残留 | 全仓扫描 | `grep -r "adopt-existing-docs" docs/ README*.md` 无命中（`.cabbage/` 内部记录除外）；`git status` 显示且仅显示本变更声明的文件 | High |
| T-2 | 工件与全树静态校验 | 静态门禁 | `cabbage validate --all` 退出码 0（无死链、无占位符、无未勾选任务） | High |
| T-3 | 站点构建 | 站点构建 | `cabbage docs build` 退出码 0，产物生成于 `docs/.vitepress/dist/` | High |
| T-4 | ARCHITECTURE.md 纠偏与源码一致 | 内容一致性 | 目录树仅含 `voiceinput-config.h`；无 `AudioSource` 配置键描述，自动选源描述与 `FindBestSourceName()` 行为一致（优先 `alsa_input.*`、排除 `.monitor`/`echoCancel`）；Route Map 为"流式/非流式"；依赖章节引用 PR #20 | High |
| T-5 | README 双语"音频设备"条目更新 | 内容一致性 | 中英文条目均不再提及 `AudioSource` 下拉框，改述自动选源，中英文语义一致 | High |
| T-6 | `docs.mapping` 生效且语法正确 | 静态门禁 | `cabbage` 各命令正常解析 config（无 invalid YAML 报错）；mapping 覆盖后 `sync`/`archive` 不再向 `docs/` 写 tech-spec/test-plan/release-plan，`requirement` 指向 `01-product/prd/` | Medium |

# Regression Coverage

- 既有 CI（ci.yml 七发行版矩阵 / release.yml）不受影响：本变更不触碰 CMake、构建脚本与 `.github/workflows/`。
- 站点既有页面（首页、各分区索引、ARCHITECTURE、v4 设计、ADR、评审、开发记录、CI/CD 分析）均不删除、不移动；T-2/T-3 的全量校验与构建即为回归证明。
- `cabbage.yml` 文档门禁行为不变：本变更后 validate/build 仍为 PR 必过项。

# Non-functional Testing

| Quality attribute | Method | Threshold |
|---|---|---|
| 可维护性（单一事实源） | 检查删除后 `docs/` 与 `.cabbage/archive/2026/adopt-existing-docs/` 无内容重复 | 重复份数 = 0（仅归档区 1 份） |
| 一致性（文档↔代码） | T-4/T-5 人工逐条核对 | 偏差条数 = 0 |

# Entry and Exit Criteria

- Entry: 变更工作区 impact/design 阶段已 verify；源码事实已核实（`AudioSource` 移除、config 目录现状、PR #20 编号）。
- Exit: T-1~T-6 全部通过并有命令输出为证；`cabbage gate tidy-docs-tree merge` 退出码 0。

# Risks

- 无自动化单测覆盖文档内容正确性，T-4/T-5 依赖人工核对——已通过在 tech-spec 中记录源码证据（文件、函数名、commit）降低主观性。
- VitePress 构建依赖 pnpm 环境，若本机依赖损坏可能误报——缓解：`docs/node_modules/` 已就绪，`cabbage doctor` 先行验证。
