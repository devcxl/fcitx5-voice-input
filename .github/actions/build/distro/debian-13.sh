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
$RUN apt-get install -y -qq \
    git curl ca-certificates g++ cmake pkg-config \
    libfcitx5core-dev libjsoncpp-dev libcurl4-openssl-dev zlib1g-dev \
    gettext dpkg-dev file \
    ${CAPTURE_PKGS:-libpipewire-0.3-dev libspa-0.2-dev libpulse-dev}

if [ "${ONNX_STRATEGY:-download}" = "system" ]; then
    $RUN apt-get install -y -qq libonnxruntime-dev
    echo "CPACK_DEBIAN_PACKAGE_SHLIBDEPS=ON" >> "${GITHUB_ENV}"
else
    echo "CPACK_DEBIAN_PACKAGE_SHLIBDEPS=OFF" >> "${GITHUB_ENV}"
fi

# SHLIBDEPS=ON 时 Depends 由 dpkg-shlibdeps 计算（覆盖此值）；
# trixie 的 jsoncpp soname 已升至 26（libjsoncpp26，不同于 bookworm 的 libjsoncpp25）
echo "CPACK_DEBIAN_PACKAGE_DEPENDS=fcitx5, libjsoncpp26, libcurl4t64, zlib1g" >> "${GITHUB_ENV}"
echo "CPACK_DEBIAN_PACKAGE_RECOMMENDS=libpipewire-0.3-0 | libpulse0" >> "${GITHUB_ENV}"
