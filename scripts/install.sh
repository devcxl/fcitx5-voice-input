#!/usr/bin/env bash
set -euo pipefail

PREFIX="${PREFIX:-/usr}"

if [ ! -f build/voice-input-addon.so ]; then
    echo "==> No build found, running build.sh first..."
    bash "$(dirname "$0")/build.sh"
fi

echo "==> Installing to ${PREFIX}..."
if [ -w "${PREFIX}" ]; then
    cmake --install build
else
    sudo cmake --install build
fi

echo "==> Installed."
