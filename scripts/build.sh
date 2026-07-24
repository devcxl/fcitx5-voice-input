#!/usr/bin/env bash
set -euo pipefail

BUILD_TYPE="${BUILD_TYPE:-Release}"
PREFIX="${PREFIX:-/usr}"

echo "==> Configuring (${BUILD_TYPE})..."
cmake -B build \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DBUILD_TESTS=OFF

echo "==> Building..."
cmake --build build -j"$(nproc)"

echo "==> Done: build/voice-input-addon.so"
