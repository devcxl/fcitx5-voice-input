#!/usr/bin/env bash
# Uninstall a source installation recorded by CMake's install manifest.
# Packaged installations (AUR/DEB/RPM) must be removed with their package manager.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/uninstall.sh [--dry-run] [--yes]

Remove files installed by `cmake --install` using BUILD_DIR/install_manifest.txt.

Environment:
  BUILD_DIR  CMake build directory (default: <repository>/build)
  PREFIX     Expected install prefix (default: prefix recorded in CMakeCache.txt)

Options:
  -n, --dry-run  List files that would be removed without changing anything.
  -y, --yes      Do not ask for confirmation when run interactively.
  -h, --help     Show this help message.

This script preserves user configuration under ~/.config/fcitx5/.
For AUR, DEB, or RPM installs, use the distribution package manager instead.
EOF
}

fail() {
    echo "Error: $*" >&2
    exit 1
}

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo_dir="$(cd -- "${script_dir}/.." && pwd -P)"
build_dir="${BUILD_DIR:-${repo_dir}/build}"
if [[ "${build_dir}" != /* ]]; then
    build_dir="${repo_dir}/${build_dir}"
fi

manifest="${build_dir}/install_manifest.txt"
cache="${build_dir}/CMakeCache.txt"
prefix="${PREFIX:-}"
dry_run=false
assume_yes=false

while (($#)); do
    case "$1" in
        -n|--dry-run)
            dry_run=true
            ;;
        -y|--yes)
            assume_yes=true
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            fail "Unknown option: $1"
            ;;
    esac
    shift
done

[[ -r "${manifest}" ]] || fail "No install manifest at ${manifest}. Reinstall from this build directory first, or use your package manager."
[[ -r "${cache}" ]] || fail "No CMake cache at ${cache}; refusing to trust the manifest without it."
grep -qx 'CMAKE_PROJECT_NAME:STATIC=fcitx5-voice-input' "${cache}" || \
    fail "${build_dir} is not a fcitx5-voice-input build directory."

if [[ -z "${prefix}" ]]; then
    prefix="$(awk -F= '$1 == "CMAKE_INSTALL_PREFIX:PATH" { print $2; exit }' "${cache}")"
fi
[[ -n "${prefix}" && "${prefix}" == /* ]] || fail "PREFIX must be an absolute path."
prefix="${prefix%/}"
[[ -n "${prefix}" ]] || fail "Refusing to uninstall from filesystem root."

mapfile -t installed_files < "${manifest}"
((${#installed_files[@]} > 0)) || fail "Install manifest is empty: ${manifest}"

for path in "${installed_files[@]}"; do
    [[ -n "${path}" ]] || continue
    [[ "${path}" == "${prefix}/"* ]] ||
        fail "Manifest path is outside PREFIX (${prefix}): ${path}. Set PREFIX to the prefix used for installation."
done

package_owner() {
    local path="$1" owner

    if command -v pacman >/dev/null 2>&1 && owner="$(pacman -Qo -- "${path}" 2>/dev/null)"; then
        printf '%s\n' "${owner}"
        return 0
    fi
    if command -v dpkg-query >/dev/null 2>&1 && owner="$(dpkg-query -S -- "${path}" 2>/dev/null)"; then
        printf '%s\n' "${owner}"
        return 0
    fi
    if command -v rpm >/dev/null 2>&1 && owner="$(rpm -qf -- "${path}" 2>/dev/null)"; then
        printf '%s\n' "${owner}"
        return 0
    fi
    return 1
}

for path in "${installed_files[@]}"; do
    [[ -e "${path}" || -L "${path}" ]] || continue
    if owner="$(package_owner "${path}")"; then
        fail "${path} is package-managed (${owner}). Use your distribution package manager instead."
    fi
done

echo "==> Files recorded for removal:"
printf '    %s\n' "${installed_files[@]}"

if "${dry_run}"; then
    echo "==> Dry run only; no files were removed."
    exit 0
fi

if [[ -t 0 && "${assume_yes}" != true ]]; then
    read -r -p "Remove these files? [y/N] " reply
    case "${reply}" in
        y|Y|yes|YES)
            ;;
        *)
            echo "==> Uninstall cancelled."
            exit 0
            ;;
    esac
fi

if [[ ${EUID} -eq 0 || -w "${prefix}" ]]; then
    privilege=()
else
    command -v sudo >/dev/null 2>&1 || fail "Write access to ${prefix} requires sudo, but sudo was not found."
    privilege=(sudo)
fi

"${privilege[@]}" rm -f -- "${installed_files[@]}"

# Only these directories are project-specific; remove them if they became empty.
for path in "${installed_files[@]}"; do
    if [[ "${path}" == */fcitx5/voice-input/models/silero_vad.onnx ]]; then
        "${privilege[@]}" rmdir -- "$(dirname -- "${path}")" 2>/dev/null || true
        "${privilege[@]}" rmdir -- "$(dirname -- "$(dirname -- "${path}")")" 2>/dev/null || true
        break
    fi
done

echo "==> Uninstalled. User configuration under ~/.config/fcitx5/ was kept."
echo "==> Restart fcitx5 (or log out and back in) to unload the removed addon."
