#include "model_paths.h"

#include <cstdlib>

namespace fcitx::model_manager {

namespace {

constexpr std::string_view kRelativeDir = "fcitx5/voice-input/models";

/// XDG Base Directory Spec：只有绝对路径才被接受，相对路径必须忽略。
bool IsAbsolutePath(std::string_view path) {
    return !path.empty() && path.front() == '/';
}

std::string Join(std::string_view base, std::string_view leaf) {
    if (base.empty()) return {};
    std::string out(base);
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    out.push_back('/');
    out.append(leaf);
    return out;
}

} // namespace

std::string DataHomeDir() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        if (IsAbsolutePath(xdg)) return xdg;
        // 相对路径按 spec 视为非法：忽略，继续用 HOME 回退。
    }
    if (const char* home = std::getenv("HOME"); home && IsAbsolutePath(home)) {
        return Join(home, ".local/share");
    }
    return {};
}

std::string DataRootDir() { return Join(DataHomeDir(), kRelativeDir); }

std::string ModelDir(std::string_view id) {
    if (!IsSafePathSegment(id)) return {};
    return Join(DataRootDir(), id);
}

std::string VersionDir(std::string_view id, std::string_view version) {
    if (!IsSafePathSegment(id) || !IsSafePathSegment(version)) return {};
    return Join(ModelDir(id), version);
}

std::string LocksDir() { return Join(DataRootDir(), ".locks"); }

std::string ModelsParentDir() {
    return Join(DataHomeDir(), "fcitx5/voice-input");
}

std::string StagingRoot() { return Join(DataRootDir(), ".staging"); }

std::string InstallMarkerPath(const std::string& versionDir) {
    if (versionDir.empty()) return {};
    return Join(versionDir, "install.json");
}

bool IsSafePathSegment(std::string_view segment) {
    if (segment.empty() || segment.size() > 200) return false;
    if (segment.front() == '.') return false; // 排除 "."、".." 与隐藏名
    for (char c : segment) {
        if (c == '/' || c == '\\' || c == '\0') return false;
    }
    return true;
}

bool IsSafeRelativePath(std::string_view relativePath) {
    if (relativePath.empty() || relativePath.size() > 200) return false;
    if (relativePath.front() == '/') return false;

    size_t start = 0;
    while (start <= relativePath.size()) {
        const size_t slash = relativePath.find('/', start);
        const size_t end =
            slash == std::string_view::npos ? relativePath.size() : slash;
        const std::string_view component = relativePath.substr(start, end - start);
        // 空组件（"a//b"、结尾 '/'）与 "."/".." 一律拒绝。
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        for (char c : component) {
            if (c == '\\' || c == '\0') return false;
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

} // namespace fcitx::model_manager
