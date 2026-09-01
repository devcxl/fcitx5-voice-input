# GitHub Workflow 多发行版构建重构调研（2026-08 修订版）

> 调研日期：2026-08-12（数据经 repology / Launchpad / GitHub API 实测核验）
> 仓库现状：main 分支 v0.4.0（2026-08-11 发布），本次重构在 `refactor/build-pipeline` 分支。
> 目的：为 workflow 重构提供依据，重点是构建流程的多发行版化。

## 1. 现状盘点（问题清单）

当前 CI 只有两个 workflow + 两个 composite action：

```
.github/workflows/build.yml     # PR/push → Ubuntu 24.04 构建 + AUR docker 构建 → artifact
.github/workflows/release.yml   # tag v* → 同上构建 → softprops 建 draft release
.github/actions/build/          # 装依赖 + 下载 onnxruntime 1.20.1 + 构建 + 链接校验 + cpack DEB
.github/actions/aur/            # prepare.sh(源码包) + Dockerfile(archlinux:base-devel) + build.sh(makepkg)
```

### 1.1 主要问题

| # | 问题 | 影响 |
|---|------|------|
| 1 | **只产出一个 Ubuntu 24.04 .deb** | Ubuntu 22.04 / Debian 12 用户装不上（glibc/libstdc++/fcitx5 ABI 差异） |
| 2 | **DEB Depends 硬编码 Ubuntu 24.04 包名**（`libcurl4t64`、`libjsoncpp25`） | Debian 12 是 `libcurl4`；t64 迁移前后包名不同，跨发行版必然错 |
| 3 | **无 RPM 产物** | Fedora / openSUSE 用户只能自己编译 |
| 4 | **build.yml 与 release.yml 复制粘贴** | 改一处忘另一处，无 reusable workflow |
| 5 | **onnxruntime 固定下载 1.20.1** | ① 版本太老（upstream 已到 1.28.0，2026-07-25 发布）；② Ubuntu 26.04 / Debian 13 / Fedora 44 / openSUSE TW / Arch 仓库**已有系统包**，下载构建既慢又无法声明运行时依赖 |
| 6 | **AUR 构建用 docker-in-actions** | 需要 Docker daemon，启动慢；直接 `container: archlinux` 更简单 |
| 7 | **无 concurrency 控制 / 无产物保留策略 / 无 provenance** | 资源浪费 + 发布资产不可审计 |
| 8 | **Ubuntu 22.04 编译不过**（隐藏约束） | CMake 硬性要求 libcurl >= 7.86.0（Volcengine WebSocket），Ubuntu 22.04 只有 7.81.0 |
| 9 | **AUR 包落后**（实测） | aur.archlinux.org 上 `fcitx5-voice-input` 由 devcxl 维护，版本停在 **0.3.1-1**（2026-07-24 更新），落后仓库 v0.4.0；发布流程没有同步 AUR |

### 1.2 现有做得好的（重构时保留）

- 录音库 dlopen + `nm -D --undefined-only` / `readelf -dW` / dlopen smoke test 的**链接校验**（对 DF_1_NOW 场景极有价值）
- `build-no-pipewire` 回归防护 job
- AUR prepare.sh 的版本推导（tag → `v0.4.0`，非 tag → `0.0.0+r<sha>`）

## 2. 同类项目调研（2026-08 复查）

### 2.1 fcitx 官方组织（最近的同类）

[fcitx/fcitx5](https://github.com/fcitx/fcitx5)、[fcitx5-chinese-addons](https://github.com/fcitx/fcitx5-chinese-addons) 的 check.yml 与去年调研**无变化**：

```yaml
runs-on: ubuntu-latest
container: archlinux:latest          # Arch 包最全最新，一次 pacman 装齐 fcitx5 全家桶
strategy:
  matrix:
    compiler: [gcc, clang]
```

- CI 只在 Arch 容器里编译 + ctest + CodeQL；**完全不做多发行版打包**，分发交给发行版维护者
- **结论不变**：官方路线对个人项目不适用（没有维护者），必须自给自足

### 2.2 wezterm（容器矩阵 + 每发行版原生包，最值得抄的模板）

[wez/wezterm](https://github.com/wez/wezterm) 2026-08 的生成式 workflow：

```
gen_ubuntu22.04 / gen_ubuntu24.04 / gen_ubuntu26.04   ← 今年新增 26.04 三件套
gen_debian12 / gen_fedora41 / gen_centos9
每个又分 _continuous（PR）/_tag（发版）两个变体
```

- 每个发行版一个 workflow，`runs-on: ubuntu-latest + container: <distro>`，**在发行版原生容器里构建原生包**，tag 时发布到自有 apt/yum 仓库（`ci/deploy.sh`）
- 注意其 fedora 还停在 41（滞后于 Fedora 43/44 现状）——矩阵需要主动维护

### 2.3 neovim（最老镜像原则 + gh CLI 发布 + 安全）

[neovim/neovim](https://github.com/neovim/neovim) 2026-08 复查：

- **仍在 ubuntu-22.04 runner 上构建**（最老兼容原则，22.04 标准支持到 2027-04）
- **发布用 `gh release create` CLI**（非 softprops），`--prerelease`/notes 模板/批量文件参数齐全
- `concurrency: cancel-in-progress`（PR 时取消旧运行）；`permissions: contents: read` 最小化
- `zizmor.yml` 静态扫描 workflow 安全（zizmor v1.29.0，2026-08 仍活跃）
- build.yml 增加 `old-cmake` job：测试 CMake 最低版本要求

### 2.4 Hyprland（C++，只 CI 不打包）

同去年：`container: archlinux` 构建 + tar.xz；release 只上传源码包，发行版包由生态承担。

### 2.5 obs-studio（Flatpak + OBS 双轨）

同去年：Flathub Flatpak 发布 + OBS `home:obsproject` 出各发行版包；action 全部 pin commit SHA。

### 2.6 input-leap（C++ 容器矩阵教科书）

同去年：<span v-pre>container: ${{ matrix.os }}</span> 矩阵（ubuntu 20.04~24.10 + debian bookworm），每容器装依赖构建；release 时才装打包工具链。

### 2.7 os-autoinst/openQA（OBS GitHub 集成）

同去年：`.obs/workflows.yml` 让 OBS 直接 watch GitHub PR，在 OBS 基础设施上构建多发行版（openSUSE/SLE）。

### 2.8 2026 年生态事实更新（与去年调研的差异）

| 项目 | 2025 结论 | 2026-08 现状 |
|------|----------|-------------|
| softprops/action-gh-release | 停止维护，建议换 gh CLI | **v3.0.2 于 2026-07-13 发布，恢复维护**（上传传输加固、复用 draft release）。gh CLI 仍是零依赖首选，但"softprops 死了"的说法过时 |
| actions/checkout | v4/v5 | **v7.0.1**（2026-07）；upload-artifact v7.0.1（2026-04） |
| ubuntu-26.04 runner | 不存在 | **preview 状态**（LTS 2026-04 发布，runner 镜像 2026-08 仍是 beta 标签）；`ubuntu-latest` 仍 = 24.04 |
| 发行版矩阵 | 无 26.04 概念 | **Ubuntu 26.04 LTS 是当前矩阵的核心新增项** |

## 3. 多发行版打包方案对比

fcitx5 addon 是**插件 .so**（装入 `$libdir/fcitx5/`，依赖 fcitx5 运行时 ABI），
**AppImage / Flatpak / snap 不适用**（非独立应用）。可选空间（结论同去年，方向不变）：

| 方案 | 覆盖发行版 | 维护成本 | 官方程度 | 代表项目 |
|------|-----------|---------|---------|---------|
| **A. GH Actions 容器矩阵**（每发行版容器出原生包） | Ubuntu/Debian/Fedora/openSUSE/Arch | 中（每发行版依赖列表） | 自建 | wezterm、input-leap |
| **B. OBS 项目**（构建在 openSUSE 基础设施） | openSUSE/Leap/SLE + 可配 Fedora/Debian/Ubuntu/Arch | 低（一次配置），但需凭据+学习 | 发行版标准设施 | obs-studio(home:obsproject)、os-autoinst |
| **C. 发行版原生仓库**：COPR(Fedora)/PPA(Ubuntu)/AUR(Arch) | 各自单发行版 | 中（每渠道账号+上传） | 官方渠道 | 大量 |
| **D. 只 CI 不打包**，交发行版维护者 | — | 零 | 最官方 | fcitx5 全家、Hyprland |
| **E. 最老镜像单包通吃** | 只能通吃同族（如 Ubuntu 22.04+） | 低 | 半官方 | neovim |

**结论不变：A 为主干，C 的 COPR/AUR 为进阶，B 为远期。**

## 4. 目标架构（重构蓝图，2026-08 版）

### 4.1 文件布局

```
.github/workflows/
├── ci.yml                # PR/push 主入口：矩阵构建（含回归防护、lint）
├── release.yml           # tag v*：构建全部产物 + 创建 draft release + 推送 AUR
└── pkg-build/            # 共享逻辑（composite action 或 reusable workflow）
    ├── action.yml        # 参数化构建：容器/依赖/onnxruntime 策略/CPack 参数/产物名
    └── distro/*.sh       # 每发行版的依赖安装 + CPack 参数（唯一需要新增维护的面）
.github/actions/aur/      # 保留，改为 container: archlinux 直跑（去掉 Docker daemon）
```

### 4.2 矩阵设计（2026-08 实测依赖数据）

```yaml
jobs:
  build:
    strategy:
      fail-fast: false
      matrix:
        include:
          - name: ubuntu-24.04        # LTS，runner 镜像 = ubuntu-latest
            container: ubuntu:24.04
            onnx: download            # 仓库无 onnxruntime → 下载 upstream release
            cpack_gen: DEB
          - name: ubuntu-26.04        # 新 LTS（2026-04），onnxruntime 首次进 Ubuntu 官方仓库
            container: ubuntu:26.04
            onnx: system              # libonnxruntime-dev 1.23.2
            cpack_gen: DEB
          - name: debian-12           # oldstable
            container: debian:12
            onnx: download            # bookworm 无 onnxruntime
            cpack_gen: DEB
          - name: debian-13           # stable (trixie)
            container: debian:13
            onnx: system              # 1.21.0
            cpack_gen: DEB
          - name: fedora-44           # 2026-04 发布，支持到 2026-11
            container: fedora:44
            onnx: system              # 1.22.2
            cpack_gen: RPM
          - name: opensuse-tumbleweed
            container: opensuse/tumbleweed
            onnx: system              # 1.27.0
            cpack_gen: RPM
          - name: arch
            container: archlinux
            onnx: system              # onnxruntime-cpu 1.28.0（与 upstream 同步）
            cpack_gen: PKGBUILD       # makepkg 出 .pkg.tar.zst
```

依赖可用性（repology + Launchpad 实测，2026-08-12）：

| 发行版 | fcitx5 | curl | onnxruntime | jsoncpp |
|--------|--------|------|-------------|---------|
| Ubuntu 22.04 | 5.0.14 | **7.81 ❌ (<7.86)** | 无 | 1.9.5 |
| Ubuntu 24.04 | 5.1.7 | 8.5 ✓ | 无（下载） | 1.9.5 |
| **Ubuntu 26.04** | 5.1.19 | 8.18 ✓ | **1.23.2 系统包 ✓**（`libonnxruntime-dev`，首次入 Ubuntu） | 1.9.6 |
| Debian 12 | 5.0.21 | 7.88 ✓ | 无（下载） | 1.9.5 |
| Debian 13 | 5.1.12 | 8.14 ✓ | 1.21.0 系统包 ✓ | 1.9.6 |
| Fedora 44 | 5.1.21 | 8.18 ✓ | 1.22.2 系统包 ✓ | 1.9.6 |
| openSUSE TW | 5.1.17 | 8.21 ✓ | 1.27.0 系统包 ✓ | 1.9.8 |
| Arch | 5.1.21 | 8.21 ✓ | 1.28.0 (onnxruntime-cpu) ✓ | 1.9.6 |

**与去年调研的差异**：
- 新增 **Ubuntu 26.04**（onnxruntime 系统包化，补上了"Ubuntu 无系统 onnxruntime"的最大缺口）
- Fedora 用 **44** 替代已 EOL 的 42；Fedora 43/44 为当前双活跃版本
- onnxruntime 下载策略版本从 1.20.1 提升到 **1.28.0**（upstream 2026-07-25 发布，仍有 linux-x64 预编译 tgz）
- Ubuntu 22.04 依然卡 curl 7.81，且 2027-04 即将 EOL → **建议直接移出矩阵**（或加 `WITH_VOLCENGINE` 选项）

### 4.3 必须处理的三个项目级约束

1. **Ubuntu 22.04 的 curl 7.81 < 7.86 硬门槛**（Volcengine WebSocket）
   → 2026 年建议：**直接排除 22.04**（2027-04 EOL，fcitx5 5.0.14 也偏老），支持范围写死"Ubuntu 24.04+ / Debian 12+ / Fedora 43+ / Arch / openSUSE"。`WITH_VOLCENGINE` 选项仍值得加（一行 CMake），但不是矩阵前提。
2. **DEB/RPM 依赖名按发行版参数化**（t64 迁移等）：
   - 关键改进：**onnx=system 的发行版把 `CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON`**——让 dpkg-shlibdeps 自动计算运行时依赖，彻底消灭硬编码包名（libcurl4t64 vs libcurl4、libjsoncpp25、libonnxruntime1.x 全自动）
   - onnx=download 的发行版（Ubuntu 24.04/Debian 12）：无法声明 Depends，保留现状 + README 注明
   - 录音后端 Recommends 按族设置：deb = `libpipewire-0.3-0 | libpulse0`；rpm = `pipewire | libpulse`（按发行版微调）
   - CMakeLists 里 `CPACK_DEBIAN_PACKAGE_DEPENDS` 改为默认值，实际值由矩阵 `-D` 传入
3. **onnxruntime 双策略**：
   - `system`：pkg-config 探测（CMake 已支持 `onnxruntime` / `libonnxruntime` 与 find_path/find_library 回退），依赖随包声明
   - `download`：保留现逻辑，版本提升为 action 输入（默认 **1.28.0**），下载走 `actions/cache`（key 含版本号）

### 4.4 每 job 的步骤模板

```text
1. checkout (submodules: recursive, fetch-depth: 1)
2. 容器内装依赖（distro/*.sh，含 rpm-build / dpkg-dev 打包工具）
3. 按 onnx 策略：pkg-config 探测 或 下载 upstream release（命中缓存则跳过）
4. cmake 配置（矩阵参数：-DCPACK_*_DEPENDS / -DSHLIBDEPS / -Wl,-z,now）
5. cmake --build
6. 链接校验（现有 nm/readelf/dlopen smoke —— 只在一个 job 跑一次，矩阵内不重复）
7. 产物冒烟：容器内 dpkg -i / rpm -i 到临时 root，验证 .so 装入 $libdir/fcitx5/
8. cpack -G <DEB|RPM>，产物重命名含发行版标识
9. upload-artifact（PR 场景 retention: 1 天）
```

### 4.5 release.yml 要点（2026-08 修订）

- **与 ci.yml 完全复用同一构建 job**（reusable workflow 或同一 composite action）
- 发布工具二选一，都成立：
  - **`gh release create` CLI**（首选，零第三方依赖；runner 自带 gh 2.97.0；neovim 在用）：`--draft --generate-notes --files "*.deb" ...`
  - **softprops/action-gh-release v3.0.2**（2026-07 恢复维护，上传传输已加固）——若用则升级到 v3，别停留在 v2
- 产物命名含发行版：`fcitx5-voice-input_0.4.0_amd64_ubuntu24.04.deb`、`fcitx5-voice-input-0.4.0-1.fc44.x86_64.rpm`、`fcitx5-voice-input-0.4.0-1-x86_64.pkg.tar.zst`
- **AUR 同步**：实测 AUR 包停在 0.3.1-1（devcxl 维护），release 时应自动推送新 PKGBUILD + 源码包（AUR SSH key secret），或至少更新仓库内 `aur/PKGBUILD` 的 pkgver
- 加分项：
  - `actions/attest-build-provenance`（SLSA v1）
  - `zizmor` 静态扫描（neovim 同款，v1.29.0 活跃）
  - `permissions: contents: write` 只给 release job
  - `concurrency: cancel-in-progress`（push/PR）
  - 双轨道可选：nightly（schedule cron）+ stable（tag），参考 neovim

### 4.6 AUR 构建改造

现状（docker run archlinux:base-devel）改为直接容器 job：

```yaml
container: archlinux
steps:
  - pacman -Syu --noconfirm base-devel git
  - useradd -m builder && chown -R builder .
  - runuser -u builder -- makepkg -f --noconfirm   # makepkg 拒绝 root
```

去掉 Docker 依赖与自维护 Dockerfile，速度与可调试性更好。

### 4.7 成本评估

- 矩阵 7 job × ~3 min，public repo 免费额度内；onnxruntime 下载缓存后可压到 ~2 min
- 每发行版维护面收敛为：`distro/*.sh` 依赖列表 + 矩阵表一行；发行版 EOL（如 Fedora 44 → 2026-11，需届时换 45/46）只是改矩阵
- **Fedora 版本节奏提示**：Fedora 每年 4 月/11 月各发布一版、每版支持 ~13 个月，矩阵应跟随"当前两活跃版本"更新

## 5. 分阶段落地

### P1 落地状态（2026-08-12，`refactor/build-pipeline` 分支）

✅ 已完成并容器实测验证（每发行版完整跑通 bootstrap → cmake → cpack → 解包冒烟）：

- 容器矩阵 7 发行版：`ci.yml` / `release.yml` 共用参数化 `.github/actions/build`
- 每发行版依赖与 CPack 参数收敛到 `.github/actions/build/distro/*.sh`（唯一维护面）
- onnxruntime 双策略：system（Ubuntu 26.04/Debian 13/Fedora 44/openSUSE/Arch）
  与 download（Ubuntu 24.04/Debian 12，版本 1.20.1 → 1.28.0，actions/cache 缓存）
- SHLIBDEPS 方案落地：实测 Ubuntu 26.04 自动生成
  `libonnxruntime1.23, libjsoncpp26, libfcitx5core7`；Debian 13 自动生成
  `libonnxruntime1.21`——手工包名错误被自动纠正，证明方案价值
- RPM：`CPACK_RPM_FILE_NAME=RPM-DEFAULT`（`-0.4.0-1.x86_64.rpm`），
  依赖由 rpmbuild 自动生成（`libFcitx5Core.so.7`、`libonnxruntime.so.1(VERS_1.27.0)`）
- AUR 构建去掉 Docker daemon：archlinux 容器内 useradd + runuser makepkg
- release 用 `softprops/action-gh-release@v3` 生成 draft release
  （v3.0.2 于 2026-07 恢复维护；gh CLI 为备选方案）
- 链接校验（nm/readelf/dlopen smoke）收敛到 verify job 单次执行

✅ 实测发现并修复的兼容性问题：

1. **curl_ws_recv const 签名**（真实 bug）：curl ≥ 8.2.0 将 `metap` 参数 const 化，
   Debian 12（7.88）编译失败。`volcengine_asr.cpp` / `realtime_asr.cpp` 按
   `LIBCURL_VERSION_NUM >= 0x080200` 条件分支修复
2. **Ubuntu 26.04 / Debian 13 jsoncpp soname = 26**（libjsoncpp26，非 25）
3. **SHLIBDEPS 需要 `file` 命令**：apt 系脚本补装
4. **Fedora onnxruntime 分包子包**：头文件在 `onnxruntime-devel`（不是 `onnxruntime`）
5. **openSUSE 包名**：`pipewire-devel`（非 libpipewire-0_3-devel）、`gettext-runtime`（非 gettext-tools）
6. **arch 容器 git dubious ownership**：workflow 加 `safe.directory`

⏳ 遗留（P2/P3）：AUR 自动推送（AUR 包仍停在 0.3.1-1）、COPR、OBS、Ubuntu PPA、
nightly 双轨道、attest-build-provenance、zizmor。

### P2/P3 路线

| 阶段 | 内容 | 验收 |
|------|------|------|
| **P2 渠道** | AUR 自动推送（补 0.3.1→0.4.0 缺口）+ COPR（fedora 官方 repo，`fedora-copr/copr-action`） | Fedora 用户 `dnf copr enable` 即装；AUR 与仓库版本同步 |
| **P3 远期** | OBS 项目（openSUSE 生态，可顺带 Fedora/Debian/Ubuntu）+ 可选 Ubuntu PPA | 发行版"官方渠道"覆盖 |

## 6. 参考资料（2026-08-12 复查）

- fcitx/fcitx5-chinese-addons `.github/workflows/check.yml`（Arch 容器 + gcc/clang 矩阵，无变化）
- wez/wezterm `gen_ubuntu26.04_tag.yml` 等（**2026 新增 26.04**；容器原生包 + 仓库发布）
- neovim/neovim `release.yml` + `zizmor.yml`（**gh release create CLI**、最老镜像 ubuntu-22.04、concurrency 控制）
- input-leap/input-leap `builds.yml`（C++ 容器矩阵教科书）
- os-autoinst/os-autoinst `.obs/workflows.yml`（OBS GitHub 集成）
- obsproject/obs-studio `publish.yaml`（Flatpak + pin SHA + 最小 permissions）
- repology.org（fcitx5 / onnxruntime / curl / jsoncpp 各发行版版本，2026-08 抓取）
- Launchpad API（Ubuntu 26.04 `libonnxruntime-dev` 1.23.2、`libfcitx5core-dev` 版本实测）
- aur.archlinux.org RPC（`fcitx5-voice-input` 0.3.1-1，maintainer=devcxl，落后于 v0.4.0）
- microsoft/onnxruntime releases（v1.28.0，2026-07-25，linux-x64 预编译 tgz 仍在发布）
- softprops/action-gh-release（v3.0.2，2026-07-13，恢复维护）
- actions/runner-images README（ubuntu-26.04 preview；ubuntu-latest = 24.04）
