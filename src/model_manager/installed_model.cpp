#include "installed_model.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

#include "fs_safe.h"
#include "model_paths.h"
#include "sha256.h"

namespace fcitx::model_manager {

const char* InstalledStateName(InstalledState state) {
    switch (state) {
        case InstalledState::Absent: return "absent";
        case InstalledState::Valid: return "valid";
        case InstalledState::Tampered: return "tampered";
        case InstalledState::Unreadable: return "unreadable";
    }
    return "unreadable";
}

InstalledCheck CheckInstalledModel(const ModelSpec& spec,
                                   const std::string& versionDir,
                                   const std::function<bool()>& cancelled) {
    InstalledCheck check;

    // 取消优先：调用方要求中止时不做任何 I/O。
    if (cancelled && cancelled()) {
        check.cancelled = true;
        check.detail = "校验已取消";
        return check;
    }

    if (versionDir.empty()) {
        check.state = InstalledState::Unreadable;
        check.detail = "版本目录为空";
        return check;
    }

    // 受控路径约束：从 models 这一级开始逐级拒绝符号链接（与 addon 侧一致），
    // 而 XDG_DATA_HOME 及其上层（含 fcitx5、voice-input）允许是用户的符号链接。
    const std::string parent = ModelsParentDir();
    if (!parent.empty() && versionDir.size() > parent.size() + 1 &&
        versionDir.compare(0, parent.size(), parent) == 0 &&
        versionDir[parent.size()] == '/') {
        const std::string relative = versionDir.substr(parent.size() + 1);
        std::string bad;
        const ChainStatus chain = CheckDirChain(parent, relative, &bad);
        if (chain == ChainStatus::Symlink) {
            check.state = InstalledState::Unreadable;
            check.detail = "模型路径包含符号链接: " + bad;
            return check;
        }
        if (chain == ChainStatus::NotDirectory) {
            check.state = InstalledState::Unreadable;
            check.detail = "模型路径中包含非目录: " + bad;
            return check;
        }
        if (chain == ChainStatus::IoError) {
            check.state = InstalledState::Unreadable;
            check.detail = "无法检查模型路径: " + bad;
            return check;
        }
        if (chain == ChainStatus::Missing) {
            check.state = InstalledState::Absent;
            check.detail = "版本目录不存在";
            return check;
        }
    }

    std::error_code ec;
    const auto status = std::filesystem::symlink_status(versionDir, ec);
    if (ec || status.type() == std::filesystem::file_type::not_found) {
        check.state = InstalledState::Absent;
        check.detail = "版本目录不存在";
        return check;
    }
    // 符号链接冒充版本目录 → 视为不可读，绝不跟随。
    if (std::filesystem::is_symlink(status)) {
        check.state = InstalledState::Unreadable;
        check.detail = "版本目录是符号链接";
        return check;
    }
    if (!std::filesystem::is_directory(status)) {
        check.state = InstalledState::Unreadable;
        check.detail = "版本路径不是目录";
        return check;
    }

    for (size_t i = 0; i < spec.fileCount; ++i) {
        const ModelFileSpec& want = spec.files[i];
        if (!IsSafeRelativePath(want.relativePath)) {
            check.state = InstalledState::Unreadable;
            check.detail = std::string("清单文件名不安全: ") + want.relativePath;
            return check;
        }

        const std::string path = versionDir + "/" + want.relativePath;

        // 以清单声明的字节数为硬上限：异常大的文件在 fstat 阶段就被拒绝，
        // 不会被完整读取（防止用户目录里的巨型文件耗尽 I/O）。
        const FileDigestResult result = DigestFile(path, want.size, cancelled);
        if (result.status == FileDigestStatus::Cancelled) {
            check.cancelled = true;
            check.detail = "校验已取消";
            return check;
        }
        switch (result.status) {
            case FileDigestStatus::Ok:
                break;
            case FileDigestStatus::TooLarge:
                check.state = InstalledState::Tampered;
                check.badFile = want.relativePath;
                check.detail = std::string("文件大于清单声明的大小: ") +
                               want.relativePath;
                return check;
            case FileDigestStatus::NotRegularFile:
                check.state = InstalledState::Tampered;
                check.badFile = want.relativePath;
                check.detail = std::string("不是普通文件: ") + want.relativePath;
                return check;
            case FileDigestStatus::OpenFailed:
                // 缺失与"打不开"都归为 tampered：从调用方看模型不可用，
                // badFile/detail 已足够定位。
                check.state = InstalledState::Tampered;
                check.badFile = want.relativePath;
                check.detail = std::string("缺少或无法以普通文件打开: ") +
                               want.relativePath;
                return check;
            case FileDigestStatus::ReadFailed:
            case FileDigestStatus::Cancelled:
                check.state = InstalledState::Unreadable;
                check.badFile = want.relativePath;
                check.detail = std::string("读取失败: ") + want.relativePath;
                return check;
        }

        if (result.size != want.size) {
            check.state = InstalledState::Tampered;
            check.badFile = want.relativePath;
            check.detail = std::string("大小不符: ") + want.relativePath +
                           "（期望 " + std::to_string(want.size) + "，实际 " +
                           std::to_string(result.size) + "）";
            return check;
        }
        if (ToHex(result.digest) != want.sha256Hex) {
            check.state = InstalledState::Tampered;
            check.badFile = want.relativePath;
            check.detail = std::string("摘要不符: ") + want.relativePath;
            return check;
        }
    }

    check.state = InstalledState::Valid;
    check.detail = "校验通过";
    return check;
}

/// 写不可信 marker。仅供快速否定与人类排查；校验一律以清单为准。
bool WriteInstallMarker(const ModelSpec& spec, const std::string& versionDir) {
    std::string json;
    json.reserve(512);
    json += "{\n";
    json += "  \"schema\": 1,\n";
    json += "  \"trusted\": false,\n";
    json += "  \"note\": \"informational only; verify against built-in catalog\",\n";
    json += "  \"id\": \"" + std::string(spec.id) + "\",\n";
    json += "  \"version\": \"" + std::string(spec.version) + "\",\n";
    json += "  \"backendId\": \"" + std::string(spec.backendId) + "\",\n";
    json += "  \"streaming\": " + std::string(spec.streaming ? "true" : "false") +
            ",\n";
    json += "  \"archiveSha256\": \"" + std::string(spec.archiveSha256Hex) +
            "\",\n";
    json += "  \"archiveSize\": " + std::to_string(spec.archiveSize) + ",\n";
    json += "  \"files\": [\n";
    for (size_t i = 0; i < spec.fileCount; ++i) {
        const ModelFileSpec& f = spec.files[i];
        json += "    {\"role\": \"" + std::string(f.role) +
                "\", \"relativePath\": \"" + std::string(f.relativePath) +
                "\", \"sha256\": \"" + std::string(f.sha256Hex) +
                "\", \"size\": " + std::to_string(f.size) + "}";
        if (i + 1 < spec.fileCount) json += ",";
        json += "\n";
    }
    json += "  ]\n}\n";

    const std::string marker = InstallMarkerPath(versionDir);
    const std::string temp = marker + ".tmp";
    const int fd = ::open(temp.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                          0644);
    if (fd < 0) return false;

    size_t offset = 0;
    while (offset < json.size()) {
        const ssize_t n = ::write(fd, json.data() + offset, json.size() - offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(temp.c_str());
            return false;
        }
        offset += static_cast<size_t>(n);
    }
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    if (!synced) {
        ::unlink(temp.c_str());
        return false;
    }
    if (::rename(temp.c_str(), marker.c_str()) != 0) {
        ::unlink(temp.c_str());
        return false;
    }
    return true;
}

} // namespace fcitx::model_manager
