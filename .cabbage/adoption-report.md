---
cabbage_stage: adoption
generated_at: 2026-09-01T20:43:37.208177+00:00
---

<!-- 手工修正：adopt 扫描把 third_party/（git submodule 上游文档，共 193 项）与
     aur/src/（AUR makepkg 构建产物副本，共 208 项）误列为项目文档。
     两者均非本项目自有文档，全部排除（keep / 不迁移）。-->

# Adoption report

Inventory of existing documentation. This report is advisory only; no files were moved.

## Action legend

| Action | Meaning |
|---|---|
| keep | Already inside the standard current-state tree; no move needed |
| migrate | Current-state document; move into the standard tree during adoption |
| import | Historical record (ADR/RFC/incident); archive as-is under the standard tree |
| review | Unclassified; a human decides whether to migrate, import, or leave it |

## Documents

| Path | Action | Category | Suggested target |
|---|---|---|---|
| `AGENTS.md` | keep | - | `-`（开发助手约定，留在仓库根目录，非站点文档） |
| `CHANGELOG.md` | keep | release | `-`（遵从开源惯例留在仓库根目录；版本发布信息见 12-release 站点页） |
| `README.md` | keep | overview | `-`（GitHub 项目主页惯例留在根目录；docs/00-overview 提供站点概览并链接） |
| `README.zh-CN.md` | keep | overview | `-`（同上，中文版） |

> 本项目 docs/ 下 10 份历史文档（architecture/adr/dev/prd/reviews）已按标准树迁移：
> 见 `docs/03-architecture/`、`docs/01-product/`、`docs/06-development/`、`docs/11-ci-cd/`。

## Adoption steps

1. Resolve each `review` row by hand, then re-run this command.
2. Move `migrate` rows into their suggested target and fix intra-project links.
3. Import `import` rows as immutable history: do not rewrite their content.
4. Record completed moves in a change record (`cabbage new feature adopt-existing-docs`).
5. Verify the current-state site (`cabbage docs build`), then enable CI gates.

The standard current-state tree is `docs/`; see `references/directory-structure.md`.