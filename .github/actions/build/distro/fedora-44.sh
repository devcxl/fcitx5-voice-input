#!/usr/bin/env bash
# Fedora 44 bootstrap. 仓库自带 onnxruntime 1.22.2（单包，含头文件）→ system 策略。
# 录音库通过 dlopen 加载，rpmbuild 无法从 ELF 自动推导其运行时依赖。
set -euo pipefail

if [ "$(id -u)" -eq 0 ]; then
    RUN=""
else
    RUN="sudo"
fi

$RUN dnf install -y \
    git curl gcc-c++ cmake pkgconf-pkg-config \
    fcitx5-devel jsoncpp-devel libcurl-devel zlib-devel \
    libarchive-devel openssl-devel \
    gettext rpm-build \
    ${CAPTURE_PKGS:-pipewire-devel pulseaudio-libs-devel}

# GUI（模型管理页）：Fedora 44 的 fcitx5-configtool 为 Qt6 →
# 装 qt6-qtbase-devel + fcitx5-qt-devel（后者同时提供 Fcitx5Qt6WidgetsAddons 的 CMake config
# 与 Qt5 变体，但未装 qt5-qtbase-devel 时 CMake 只构建 Qt6 变体）。
# dbus-daemon 提供 dbus-run-session（model_gui_poll_test 用），tests job 需要。
$RUN dnf install -y qt6-qtbase-devel fcitx5-qt-devel dbus-daemon

# 注意：Fedora 的 onnxruntime 分包子包——onnxruntime（运行时）+
# onnxruntime-devel（头文件/libonnxruntime.so symlink）
if [ "${ONNX_STRATEGY:-download}" = "system" ]; then
    $RUN dnf install -y onnxruntime-devel
fi

echo "CPACK_RPM_PACKAGE_REQUIRES=pulseaudio-libs" >> "${GITHUB_ENV}"

# 打包冒烟断言用：本发行版实际构建的 Qt 主版本（跟随 fcitx5-config-qt）。
echo "VOICE_INPUT_GUI_QT_MAJOR=6" >> "${GITHUB_ENV}"
