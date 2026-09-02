# 快速安装指南

fcitx5-voice-input 支持主流 Linux 发行版，提供 AUR 源码包、官方 GitHub Release 预编译包以及从源码手动编译安装等多种方式。

---

## 方式一：Arch Linux (AUR)

如果您使用的是 Arch Linux 或其衍生发行版（如 Manjaro、EndeavourOS），可以通过 AUR 助手一键安装：

```bash
# 使用 yay 安装
yay -S fcitx5-voice-input

# 或使用 paru 安装
paru -S fcitx5-voice-input
```

也可以通过 `makepkg` 手动构建：

```bash
git clone https://aur.archlinux.org/fcitx5-voice-input.git
cd fcitx5-voice-input
makepkg -si
```

---

## 方式二：预编译安装包 (GitHub Releases)

项目的 CI 流水线会自动为多个主流 Linux 发行版构建原生安装包。请前往 [Releases 页面](https://github.com/devcxl/fcitx5-voice-input/releases) 下载对应系统的最新版本安装包。

### 1. Ubuntu / Debian (`.deb`)

支持 Ubuntu 24.04+、Debian 12+：

```bash
# 下载对应的 .deb 包后执行安装
sudo apt install ./fcitx5-voice-input_*_amd64.deb
```

### 2. Fedora (`.rpm`)

支持 Fedora 40+：

```bash
# 下载对应的 .rpm 包后执行安装
sudo dnf install ./fcitx5-voice-input-*.x86_64.rpm
```

### 3. openSUSE Tumbleweed / Leap (`.rpm`)

```bash
sudo zypper install ./fcitx5-voice-input-*.x86_64.rpm
```

---

## 方式三：源码手动编译安装

当您的发行版未提供预编译包，或者您需要对插件进行开发和调试时，可选择从源码编译。

### 1. 安装编译依赖

fcitx5-voice-input 依赖 Fcitx5 核心头文件、JSON 解析库、curl、zlib、ONNX Runtime 以及音频采集后端（PulseAudio 或 PipeWire，至少具备其一）。

::: code-group

```bash [Arch Linux]
sudo pacman -S --needed \
    base-devel cmake git extra-cmake-modules \
    fcitx5 fcitx5-configtool jsoncpp curl zlib \
    pulseaudio pipewire onnxruntime-cpu
```

```bash [Ubuntu / Debian]
sudo apt update
sudo apt install -y \
    build-essential cmake git extra-cmake-modules \
    fcitx5 fcitx5-modules-dev libfcitx5core-dev fcitx5-config-qt \
    libjsoncpp-dev libcurl4-openssl-dev zlib1g-dev \
    libpulse-dev libpipewire-0.3-dev libonnxruntime-dev
```

```bash [Fedora]
sudo dnf install -y \
    gcc-c++ cmake git extra-cmake-modules \
    fcitx5-devel fcitx5-configtool jsoncpp-devel \
    libcurl-devel zlib-devel pulseaudio-libs-devel \
    pipewire-devel onnxruntime-devel
```

```bash [openSUSE]
sudo zypper install -y \
    gcc-c++ cmake git extra-cmake-modules \
    fcitx5-devel fcitx5-configtool jsoncpp-devel \
    libcurl-devel zlib-devel libpulse-devel \
    pipewire-devel onnxruntime-devel
```

:::

> **说明**：音频采集库（`libpulse-simple` 与 `libpipewire-0.3`）在运行时均采用 `dlopen` 延迟加载设计，无强制硬链接。即使系统仅安装了其中一个音频库，插件也能正常编译并优雅降级。

---

### 2. 获取源码与初始化子模块

插件的 VAD 模块依赖 Silero ONNX 模型，模型通过 Git Submodule 进行管理：

```bash
# 克隆仓库
git clone https://github.com/devcxl/fcitx5-voice-input.git
cd fcitx5-voice-input

# 初始化子模块（下载 Silero VAD 模型文件，必需步骤）
git submodule update --init --recursive
```

---

### 3. CMake 配置与编译

```bash
# 生成构建配置
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTS=OFF

# 开始编译
cmake --build build -j"$(nproc)"
```

#### CMake 常用编译选项

| 选项名 | 默认值 | 描述说明 |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | 构建类型（`Release` / `Debug` / `RelWithDebInfo`） |
| `CMAKE_INSTALL_PREFIX` | `/usr` | 安装前缀（Fcitx5 插件通常要求安装至 `/usr`） |
| `BUILD_TESTS` | `OFF` | 是否编译测试用例 |
| `ONNXRUNTIME_ROOT` | (系统路径) | 手动指定自定义 ONNX Runtime 安装目录 |

---

### 4. 安装到系统

```bash
sudo cmake --install build --prefix /usr
```

---

## 安装后生效

完成安装后，需要重启 Fcitx5 进程使新注册的插件模块生效：

```bash
# 重新加载并重启 fcitx5
fcitx5 -r -d
```

接下来可参考 [入门指南](/00-overview/getting-started) 在 `fcitx5-configtool` 中启用并配置 **Voice Input** 输入法。
