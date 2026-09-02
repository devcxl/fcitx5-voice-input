---
change: product-prd-v1
cabbage_stage: tests
change_type: feature
---

# Strategy

本变更为纯文档交付，无运行代码改动，因此以**文档验收测试**为主题：验证 PRD 内容与代码事实的一致性、生命周期门禁、链接完整性与站点构建。

| Level | Scope | Test seam | Owner |
|---|---|---|---|
| 静态验证 | PRD 占位符/链接/heading 合规 | `cabbage verify product-prd-v1 <stage>` | 变更作者 |
| 全量校验 | 全仓 Markdown 链接完整性 | `cabbage validate --all` | 变更作者 |
| 站点构建 | VitePress 编译 + 导航链接有效 | `pnpm --dir docs build` | 变更作者 |
| 事实一致性 | 需求条目 ↔ `src/addon/` 代码事实逐个对照 | 人工对照审阅（PRD 正文） | 变更作者/评审 |

# Test Environment and Data

- 环境：本机 git worktree + `.cabbage/tooling` Python 环境 + docs 项目 `pnpm` 依赖（均已就绪）。
- 数据：`src/addon/` v0.5.0 源码作为事实基准；PRD 中引用配置项/默认值与 `voiceinput-config.h`、`CMakeLists.txt` 对照。
- 隔离：不依赖外部 ASR/LLM 服务、无凭据、不触碰运行期音频。

# Cases

| ID | Scenario | Level | Expected result | Priority |
|---|---|---|---|---|
| T-1 | `cabbage verify` requirement/impact/design/tests/implementation 各阶段 | 静态验证 | 全部通过，模板占位符无残留 | High |
| T-2 | `cabbage validate --all` | 静态验证 | 输出 VALID，无 broken link | High |
| T-3 | `pnpm build`（docs 目录） | 站点构建 | build complete，渲染正常 | High |
| T-4 | 24 条需求与 `src/addon/` 事实逐个对照（POST ROLL 验收） | 事实一致性 | 每条需求可在代码中找到支撑事实，无外推 | High |
| T-5 | 未实现 roadmap 功能未混入要求集 | 事实一致性 | 本地 ASR/Command/场景/热词仅出现在 Out of Scope/Future | High |
| T-6 | 新 PRD 已挂载导航（sidebar + 首页表格）且与既有 PRD 无冲突 | 站点构建 | 链接可达，`add-input-method-icon.md` 保留 | Medium |

# Regression Coverage

- 既有功能级 PRD `add-input-method-icon.md` 与 `out-of-scope.md`：验证未被覆写或删除、链接仍可达。
- `.vitepress/config.ts` 与 `docs/README.md` 改动仅增不改：既有导航条目全量保留。

# Non-functional Testing

| Quality attribute | Method | Threshold |
|---|---|---|
| 正确性（文档与代码一致） | 人工逐条对照（T-4、T-5） | 100% 需求可溯源到代码事实 |
| 可验证性 | 每条需求使用 SHALL/MUST 措辞 | 全部需求满足可测试写法，无模糊词 |

# Entry and Exit Criteria

- Entry：PRD 已 `verify` 通过；`src/addon/` 事实基线已审读。
- Exit：以上 T-1~T-5 全部通过与 T-6 确认；`cabbage archive` 完成，文档树同步产物与工作区已收敛。

# Risks

- 覆盖盲区：文档一致性依赖人工对照，长期演进行（如新后端）可能未同步——缓解：PRD 定位现状基线，代码变更随生命周期同步更新（见 impact.md）。
- 无自动化测试守护需求文本：缓解：静态门禁（verify/validate/docs build）在 Cabbage CI 执行，内容一致性由评审把关。