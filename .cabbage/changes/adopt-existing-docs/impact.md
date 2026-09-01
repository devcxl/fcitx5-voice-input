---
change: adopt-existing-docs
cabbage_stage: impact
change_type: feature
---

# Change Summary

将 docs/ 下 10 份既有文档迁移至 Cabbage 标准编号树并修复链接；定制 VitePress 项目站点；同步 ARCHITECTURE.md / v4 设计文档 / README 双语 / AGENTS.md 至当前代码（v4 会话模型 + 3 大 ASR 引擎）；建立 adoption 基线变更。影响对象：文档读者、文档维护者、docs CI 门禁。

# Impact Matrix

| Area | Impact | Notes |
|---|---|---|
| Product | Yes | 文档分区/路径变更，站点首页与索引建立 |
| Architecture | No | 代码架构不变；本文档反映架构现状 |
| API | No | 无 API 变更 |
| Database | No | |
| Security | No | |
| Testing | Yes | 新增 docs 静态门禁与站点构建验证 |
| Deployment | No | 新增 GitHub Pages 部署 workflow（cabbage.yml），不影响现有构建产物 |
| Operations | No | |
| Data | No | |
| Performance | No | |

# Impact Details

- **Product**: 文档路径从 5 个非标准目录收敛到 4 个编号分区（01/03/06/11），站点可导航可搜索；根 README/CHANGELOG 位置不变。
- **Testing**: `cabbage validate` + `cabbage docs build` 成为 PR 门禁；占位符/坏链将被 CI 拒绝。
- **Deployment**: cabbage.yml 新增 GitHub Pages 部署（仅 main push 触发）；若无 Pages 配置可裁剪，不影响现有 ci/release。

# Risks

| Risk | Likelihood | Impact | Mitigation | Owner |
|---|---|---|---|---|
| cabbage.yml 引用仓库不存在的文件（requirements.txt、cabbage_cli、tests/） | High | CI job 失败 | T-4 中核验并修正/裁剪 workflow | maintainer |
| 站点构建拉取 pnpm 依赖缓慢 | Low | CI 时间增加 | pnpm cache action 已配置 | maintainer |

# Documentation Updates

- 迁移映射、排除决策：`.cabbage/adoption-report.md`
- 变更记录：`.cabbage/changes/adopt-existing-docs/`（本变更）
- 站点：`docs/README.md`、各分区 `index.md`、`docs/.vitepress/config.ts`
- 内容同步：`docs/03-architecture/system-design/ARCHITECTURE.md`、`v4-asr-session-model.md`、`README.md`、`README.zh-CN.md`、`AGENTS.md`