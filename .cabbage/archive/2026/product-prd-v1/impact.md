---
change: product-prd-v1
cabbage_stage: impact
change_type: feature
---

# Change Summary

从 `src/addon/`（v0.5.0）代码反推形成一份产品级 PRD，作为语音输入插件的产品现状基线文档。该 PRD 覆盖音频捕获、VAD 分段、ASR 多后端、LLM 后处理、结果保序、Fcitx5 UI、安全隐私、打包分发等 9 大能力域，包含 24 条可测试需求与 6 个验收场景。

纯文档交付，不修改任何源代码、构建脚本或运行时行为。新增产物：`.cabbage/changes/product-prd-v1/prd.md`（工作区）与 `docs/01-product/prd/product-prd-v1.md`（正式文档树），并更新站点导航（sidebar + 首页目录表格）。受影响对象：产品文档读者、后续需求变更与回归测试的执行者。

# Impact Matrix

| Area | Impact | Notes |
|---|---|---|
| Product | Yes | 确立产品现状基线 PRD，作为后续需求变更/回归的参照 |
| Architecture | No | 不改变任何代码结构或运行行为 |
| API | No | 无接口/协议变更 |
| Database | No | 无数据存储变更 |
| Security | No | 仅文档；不触碰凭据或传输逻辑 |
| Testing | Yes | PRD 验收场景可作为未来回归/验收测试需求来源 |
| Deployment | No | 无打包/安装变更 |
| Operations | No | 无运行期行为变更 |
| Data | No | 无数据模型变更 |
| Performance | No | 无性能影响 |

# Impact Details

- **Product**：新增 `docs/01-product/prd/product-prd-v1.md`，定义了产品的 Goal、User/Use Cases、Scope（含 Out of Scope）、24 条 Requirements、6 个 Acceptance Scenarios、Success Metrics 与 Risks。它是产品当前形态的唯一权威现状描述，现有 `add-input-method-icon.md` 功能级 PRD 继续保留并作为历史单点需求记录关联。
- **Testing**：PRD 中 6 个验收场景（免按即说、流式增量、LLM 修正、音频回退、防误触、后端热切换）可直接转录为未来手工/自动化验收用例；24 条需求可作为回归覆盖映射来源。

# Risks

| Risk | Likelihood | Impact | Mitigation | Owner |
|---|---|---|---|---|
| 反推基线随代码演进而过时 | Medium | PRD 与实现脱节，误导读者 | PRD 定位为"现状基线"，代码/产品变更时按生命周期同步更新；frontmatter 保留 origin_change 追踪 | 维护者 |
| 与现有功能级 PRD 内容重复/冲突 | Low | 文档多源事实不一致 | 现有图标 PRD 为单点历史记录，产品级 PRD 为总览，二者范围明确无重叠 | 维护者 |

# Documentation Updates

- 新增 `docs/01-product/prd/product-prd-v1.md`（产品级现状 PRD）。
- 更新 `docs/.vitepress/config.ts`：01-product sidebar 加入产品级 PRD 条目。
- 更新 `docs/README.md`：文档目录表格 01-product 行指向产品级 PRD 并列出相关子项。