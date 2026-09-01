# CI/CD

## 构建与发布

- [多发行版构建分析](multi-distro-build-analysis.md) — 7 发行版容器矩阵（ubuntu/debian/fedora/opensuse/arch）调研与现状

## Workflow 说明

- `ci.yml`：PR/push 触发，多发行版原生包构建 + 链接校验 + build-no-pipewire 回归防护
- `release.yml`：tag `v*` 触发，复用构建矩阵生成 draft release（含 AUR 源码包）
- `cabbage.yml`：文档变更验证 + GitHub Pages 站点部署