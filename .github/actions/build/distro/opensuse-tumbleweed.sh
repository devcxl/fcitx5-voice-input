#!/usr/bin/env bash
# openSUSE Tumbleweed bootstrap. 仓库自带 onnxruntime 1.27.0 → system 策略。
set -euo pipefail

if [ "$(id -u)" -eq 0 ]; then
    RUN=""
else
    RUN="sudo"
fi

# Tumbleweed 为滚动发行版，容器内 zypper 元数据可能过期（镜像 404），先 refresh
$RUN zypper --non-interactive --gpg-auto-import-keys refresh
$RUN zypper --non-interactive install -y \
    git curl gcc-c++ cmake pkg-config \
    fcitx5-devel jsoncpp-devel libcurl-devel zlib-devel \
    gettext-runtime rpm-build \
    ${CAPTURE_PKGS:-pipewire-devel libpulse-devel}

if [ "${ONNX_STRATEGY:-download}" = "system" ]; then
    $RUN zypper --non-interactive install -y onnxruntime-devel
fi
