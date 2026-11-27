#!/usr/bin/env bash
# Ubuntu 24.04 LTS bootstrap: install build deps + export per-distro CPack params.
# Runs as root in containers; falls back to sudo on hosted runners.
set -euo pipefail

if [ "$(id -u)" -eq 0 ]; then
    RUN=""
else
    RUN="sudo"
fi

export DEBIAN_FRONTEND=noninteractive
$RUN apt-get update -qq
# GUI（模型管理页）按发行版配置工具的 Qt 主版本安装：
# 24.04 的 fcitx5-config-qt 为 Qt5 → 只装 Qt5 + Fcitx5Qt5WidgetsAddons（libfcitx5-qt-dev）。
# Qt6 WidgetsAddons 在 24.04 也存在，但装进来会产出无人加载的 qt6/ 插件并拖入 Qt6 运行时依赖。
$RUN apt-get install -y -qq \
    git curl ca-certificates g++ cmake pkg-config \
    libfcitx5core-dev libjsoncpp-dev libcurl4-openssl-dev zlib1g-dev \
    libarchive-dev libssl-dev qtbase5-dev libfcitx5-qt-dev \
    gettext dpkg-dev file dbus-daemon \
    ${CAPTURE_PKGS:-libpipewire-0.3-dev libspa-0.2-dev libpulse-dev}

# Ubuntu 24.04 官方仓库无 onnxruntime → download 策略（action 下载 upstream release）
if [ "${ONNX_STRATEGY:-download}" = "system" ]; then
    $RUN apt-get install -y -qq libonnxruntime-dev
    echo "CPACK_DEBIAN_PACKAGE_SHLIBDEPS=ON" >> "${GITHUB_ENV}"
else
    echo "CPACK_DEBIAN_PACKAGE_SHLIBDEPS=OFF" >> "${GITHUB_ENV}"
fi

# t64 迁移后的运行时包名（Ubuntu 24.04 起 libcurl4t64；libjsoncpp25 为 jsoncpp soname）
# GUI 运行时：libfcitx5-qt1（Qt5 WidgetsAddons，连带 libqt5* 运行时）、
# libarchive13t64（模型安装器）、libssl3t64（addon 与安装器的 OpenSSL Crypto）。
# SHLIBDEPS=ON 时这些由 dpkg-shlibdeps 从 ELF NEEDED 自动推导，只写基础集避免重复条目。
if [ "${ONNX_STRATEGY:-download}" = "system" ]; then
    export CPACK_DEBIAN_PACKAGE_DEPENDS="fcitx5, libjsoncpp25, libcurl4t64, zlib1g, libpulse0"
else
    export CPACK_DEBIAN_PACKAGE_DEPENDS="fcitx5, libjsoncpp25, libcurl4t64, zlib1g, libpulse0, libfcitx5-qt1, libarchive13t64, libssl3t64"
fi
echo "CPACK_DEBIAN_PACKAGE_DEPENDS=${CPACK_DEBIAN_PACKAGE_DEPENDS}" >> "${GITHUB_ENV}"
echo "CPACK_DEBIAN_PACKAGE_RECOMMENDS=libpipewire-0.3-0" >> "${GITHUB_ENV}"

# 打包冒烟断言用：本发行版实际构建的 Qt 主版本（跟随 fcitx5-config-qt）。
echo "VOICE_INPUT_GUI_QT_MAJOR=5" >> "${GITHUB_ENV}"
