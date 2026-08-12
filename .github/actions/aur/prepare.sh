#!/usr/bin/env bash
set -euo pipefail

PKG="fcitx5-voice-input"
TAG="${GITHUB_REF_NAME}"

# Derive version: use tag if it's a release tag, otherwise use git SHA.
# pkgver only allows: alphanumeric, period, underscore, plus.
if [[ "${TAG}" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    VER="${TAG#v}"
else
    VER="0.0.0+r$(git rev-parse --short HEAD)"
fi

mkdir -p dist/aur/src

# Source tarball with submodules (top dir = pkgname-pkgver, matching GitHub archive convention)
SRC_DIR="${PKG}-${VER}"
mkdir -p "${SRC_DIR}"
git archive HEAD | tar -x -C "${SRC_DIR}"
cp -a third_party "${SRC_DIR}/"
tar -czf "dist/aur/src/${SRC_DIR}.tar.gz" "${SRC_DIR}"
rm -rf "${SRC_DIR}"

# PKGBUILD with version and sha256
SHA256=$(sha256sum "dist/aur/src/${PKG}-${VER}.tar.gz" | cut -d' ' -f1)
sed -e "s/pkgver=.*/pkgver=${VER}/" \
    -e "s/sha256sums=('SKIP')/sha256sums=('${SHA256}')/" \
    aur/PKGBUILD > dist/aur/PKGBUILD

echo "tarball=dist/aur/src/${PKG}-${VER}.tar.gz" >> "${GITHUB_OUTPUT}"
echo "pkgbuild=dist/aur/PKGBUILD" >> "${GITHUB_OUTPUT}"
