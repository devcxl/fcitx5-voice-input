#!/usr/bin/env bash
# Build Arch package inside an archlinux container (no Docker daemon).
# The workflow job must run with container: archlinux; makepkg refuses root,
# so this script creates a builder user and runs makepkg via runuser.
set -euo pipefail

PKG="fcitx5-voice-input"
WORK_DIR="dist/aur/pkg"
OUT_DIR="dist/aur"

mkdir -p "${WORK_DIR}"

# Feed PKGBUILD + source tarball into build directory (makepkg reuses the local
# tarball when the filename matches the source= entry)
cp "${OUT_DIR}/PKGBUILD" "${WORK_DIR}/"
cp "${OUT_DIR}"/src/*.tar.gz "${WORK_DIR}/"

# makepkg must not run as root
if ! id builder &>/dev/null; then
    useradd -m builder
fi
chown -R builder:builder "${PWD}/${WORK_DIR}"

runuser -u builder -- sh -c "cd '${PWD}/${WORK_DIR}' && makepkg -f --noconfirm"

PKGFILE=$(ls "${WORK_DIR}"/*.pkg.tar.zst 2>/dev/null | head -1 || true)
if [ -n "${PKGFILE}" ]; then
    PKGINFO=$(pacman -Qip "${PKGFILE}")
    if ! grep -q 'libpulse' <<<"${PKGINFO}"; then
        echo "::error::Arch package is missing required libpulse dependency"
        exit 1
    fi
    if ! grep -q 'pipewire' <<<"${PKGINFO}"; then
        echo "::error::Arch package is missing PipeWire fallback optdependency"
        exit 1
    fi
    cp "${PKGFILE}" "${OUT_DIR}/"
    echo "pkgfile=${PKGFILE}" >> "${GITHUB_OUTPUT}"
else
    echo "::error::makepkg produced no .pkg.tar.zst — check AUR build logs"
    echo "pkgfile=" >> "${GITHUB_OUTPUT}"
    exit 1
fi
