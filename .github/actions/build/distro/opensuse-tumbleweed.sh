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
$RUN zypper --non-interactive install -y --allow-downgrade \
    git curl gcc-c++ cmake pkg-config \
    fcitx5-devel jsoncpp-devel libcurl-devel zlib-devel \
    libarchive-devel libopenssl-devel \
    gettext-runtime rpm-build \
    ${CAPTURE_PKGS:-pipewire-devel libpulse-devel}

# GUI（模型管理页）：Tumbleweed 的 fcitx5-configtool 为 Qt6 →
# 装 qt6-base-devel + fcitx5-qt-devel（后者提供 Fcitx5Qt6WidgetsAddons 的 CMake config；
# 未装 libqt5-qtbase-devel 时 CMake 只构建 Qt6 变体）。
# dbus-1-daemon 提供 dbus-run-session（model_gui_poll_test 用），tests job 需要。
$RUN zypper --non-interactive install -y --allow-downgrade \
    qt6-base-devel fcitx5-qt-devel dbus-1-daemon

if [ "${ONNX_STRATEGY:-download}" = "system" ]; then
    $RUN zypper --non-interactive install -y --allow-downgrade onnxruntime-devel
fi

echo "CPACK_RPM_PACKAGE_REQUIRES=libpulse0" >> "${GITHUB_ENV}"

# 打包冒烟断言用：本发行版实际构建的 Qt 主版本（跟随 fcitx5-config-qt）。
echo "VOICE_INPUT_GUI_QT_MAJOR=6" >> "${GITHUB_ENV}"
