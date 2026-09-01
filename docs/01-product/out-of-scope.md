# Out of Scope

记录需求访谈过程中明确排除的功能及原因。

---

## 2026-07-13 | 输入法图标 — 多尺寸 PNG 图标

- **排除项**：为 `48x48`、`64x64`、`128x128` 等尺寸生成 PNG 图标
- **原因**：SVG 矢量格式已覆盖所有尺寸；额外多套 PNG 增加维护负担无实际收益
- **提议人**：原始 Issue 作者

---

## 2026-07-13 | 输入法图标 — gtk-update-icon-cache

- **排除项**：`cmake --install` 后自动执行 `gtk-update-icon-cache`
- **原因**：图标缓存刷新应由发行版包管理器的 post-install 钩子处理，不应在 CMake install 步骤中执行
- **提议人**：原始 Issue 作者
