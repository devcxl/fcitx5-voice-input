#!/usr/bin/env bash
# Ubuntu 26.04 LTS bootstrap. onnxruntime 首次进入 Ubuntu 官方仓库（1.23.2），
# 走 system 策略并开启 dpkg-shlibdeps 自动计算运行时依赖。
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

# SHLIBDEPS=ON 时，dpkg-shlibdeps 自动计算的 Depends 会与此值合并；
# 注意 Ubuntu 26.04 jsoncpp soname 升至 26（libjsoncpp26，不同于 24.04 的 libjsoncpp25）
echo "CPACK_DEBIAN_PACKAGE_DEPENDS=fcitx5, libjsoncpp26, libcurl4t64, zlib1g, libpulse0" >> "${GITHUB_ENV}"
echo "CPACK_DEBIAN_PACKAGE_RECOMMENDS=libpipewire-0.3-0" >> "${GITHUB_ENV}"
