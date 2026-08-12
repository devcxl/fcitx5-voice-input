#!/usr/bin/env bash
# Fedora 44 bootstrap. 仓库自带 onnxruntime 1.22.2（单包，含头文件）→ system 策略。
# RPM 运行时依赖由 rpmbuild 自动生成（.so SONAME → 提供包），无需手工维护。
set -euo pipefail

if [ "$(id -u)" -eq 0 ]; then
    RUN=""
else
    RUN="sudo"
fi

$RUN dnf install -y \
    git curl gcc-c++ cmake pkgconf-pkg-config \
    fcitx5-devel jsoncpp-devel libcurl-devel zlib-devel \
    gettext rpm-build \
    ${CAPTURE_PKGS:-pipewire-devel pulseaudio-libs-devel}

# 注意：Fedora 的 onnxruntime 分包子包——onnxruntime（运行时）+
# onnxruntime-devel（头文件/libonnxruntime.so symlink）
if [ "${ONNX_STRATEGY:-download}" = "system" ]; then
    $RUN dnf install -y onnxruntime-devel
fi
