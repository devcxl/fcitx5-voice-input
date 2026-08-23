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
# 容器默认带 busybox-gawk，与 rpm-build 依赖的 gawk 包冲突（滚动版镜像布局
# 差异），显式移除以消除依赖解析冲突
$RUN zypper --non-interactive remove -y busybox-gawk || true
$RUN zypper --non-interactive install -y \
    git curl gcc-c++ cmake pkg-config \
    fcitx5-devel jsoncpp-devel libcurl-devel zlib-devel \
    gettext-runtime rpm-build \
    ${CAPTURE_PKGS:-pipewire-devel libpulse-devel}

if [ "${ONNX_STRATEGY:-download}" = "system" ]; then
    $RUN zypper --non-interactive install -y onnxruntime-devel
fi

echo "CPACK_RPM_PACKAGE_REQUIRES=libpulse0" >> "${GITHUB_ENV}"
