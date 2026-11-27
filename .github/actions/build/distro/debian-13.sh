#!/usr/bin/env bash
# Debian 13 (trixie) bootstrap. 仓库自带 onnxruntime 1.21.0 → system 策略。
set -euo pipefail

if [ "$(id -u)" -eq 0 ]; then
    RUN=""
else
    RUN="sudo"
fi

export DEBIAN_FRONTEND=noninteractive
$RUN apt-get update -qq
# trixie 的 fcitx5-config-qt 为 Qt6 → 装 Qt6 + Fcitx5Qt6WidgetsAddons（libfcitx5-qt6-dev）。
$RUN apt-get install -y -qq \
    git curl ca-certificates g++ cmake pkg-config \
    libfcitx5core-dev libjsoncpp-dev libcurl4-openssl-dev zlib1g-dev \
    libarchive-dev libssl-dev qt6-base-dev libfcitx5-qt6-dev \
    gettext dpkg-dev file dbus-daemon \
    ${CAPTURE_PKGS:-libpipewire-0.3-dev libspa-0.2-dev libpulse-dev}

if [ "${ONNX_STRATEGY:-download}" = "system" ]; then
    $RUN apt-get install -y -qq libonnxruntime-dev
    echo "CPACK_DEBIAN_PACKAGE_SHLIBDEPS=ON" >> "${GITHUB_ENV}"
else
    echo "CPACK_DEBIAN_PACKAGE_SHLIBDEPS=OFF" >> "${GITHUB_ENV}"
fi

# SHLIBDEPS=ON 时，dpkg-shlibdeps 自动计算的 Depends 会与此值合并；
# trixie 的 jsoncpp soname 已升至 26（libjsoncpp26，不同于 bookworm 的 libjsoncpp25）
export CPACK_DEBIAN_PACKAGE_DEPENDS="fcitx5, libjsoncpp26, libcurl4t64, zlib1g, libpulse0"
if [ "${ONNX_STRATEGY:-download}" != "system" ]; then
    # download 策略（SHLIBDEPS=OFF）下手写 GUI 运行时依赖：
    # libfcitx5-qt6-1（Qt6 WidgetsAddons）、libarchive13t64、libssl3t64。
    CPACK_DEBIAN_PACKAGE_DEPENDS="${CPACK_DEBIAN_PACKAGE_DEPENDS}, libfcitx5-qt6-1, libarchive13t64, libssl3t64"
fi
# SHLIBDEPS=ON（system 策略）时上述 GUI 依赖由 dpkg-shlibdeps 从 ELF NEEDED 推导。
echo "CPACK_DEBIAN_PACKAGE_DEPENDS=${CPACK_DEBIAN_PACKAGE_DEPENDS}" >> "${GITHUB_ENV}"
echo "CPACK_DEBIAN_PACKAGE_RECOMMENDS=libpipewire-0.3-0" >> "${GITHUB_ENV}"

# 打包冒烟断言用：本发行版实际构建的 Qt 主版本（跟随 fcitx5-config-qt）。
echo "VOICE_INPUT_GUI_QT_MAJOR=6" >> "${GITHUB_ENV}"
