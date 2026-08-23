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
    gettext rpm-build \
    ${CAPTURE_PKGS:-pipewire-devel pulseaudio-libs-devel}

# 注意：Fedora 的 onnxruntime 分包子包——onnxruntime（运行时）+
# onnxruntime-devel（头文件/libonnxruntime.so symlink）
if [ "${ONNX_STRATEGY:-download}" = "system" ]; then
    $RUN dnf install -y onnxruntime-devel
fi

echo "CPACK_RPM_PACKAGE_REQUIRES=pulseaudio-libs" >> "${GITHUB_ENV}"
