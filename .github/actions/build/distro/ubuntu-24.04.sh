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
$RUN apt-get install -y -qq \
    git curl ca-certificates g++ cmake pkg-config \
    libfcitx5core-dev libjsoncpp-dev libcurl4-openssl-dev zlib1g-dev \
    gettext dpkg-dev file \
    ${CAPTURE_PKGS:-libpipewire-0.3-dev libspa-0.2-dev libpulse-dev}

# Ubuntu 24.04 官方仓库无 onnxruntime → download 策略（action 下载 upstream release）
if [ "${ONNX_STRATEGY:-download}" = "system" ]; then
    $RUN apt-get install -y -qq libonnxruntime-dev
    echo "CPACK_DEBIAN_PACKAGE_SHLIBDEPS=ON" >> "${GITHUB_ENV}"
else
    echo "CPACK_DEBIAN_PACKAGE_SHLIBDEPS=OFF" >> "${GITHUB_ENV}"
fi

# t64 迁移后的运行时包名（Ubuntu 24.04 起 libcurl4t64；libjsoncpp25 为 jsoncpp soname）
echo "CPACK_DEBIAN_PACKAGE_DEPENDS=fcitx5, libjsoncpp25, libcurl4t64, zlib1g" >> "${GITHUB_ENV}"
echo "CPACK_DEBIAN_PACKAGE_RECOMMENDS=libpipewire-0.3-0 | libpulse0" >> "${GITHUB_ENV}"
