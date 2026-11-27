#include "sherpa_model_verify.h"

#include <cerrno>
#include <sys/stat.h>

#include "model_paths.h"
#include "sha256.h"

namespace fcitx {

namespace {

constexpr const char* kRoleEncoder = "encoder";
constexpr const char* kRoleDecoder = "decoder";
constexpr const char* kRoleJoiner = "joiner";
constexpr const char* kRoleTokens = "tokens";

bool SameName(const char* a, const char* b) {
    return a && b && std::string_view(a) == std::string_view(b);
}

/// 清单自身合法性：只有通过这里，后续路径拼接才安全。
bool IsWellFormedSpec(const model_manager::ModelSpec& spec) {
    if (!spec.id || !spec.version) return false;
    if (!model_manager::IsSafePathSegment(spec.id)) return false;
    if (!model_manager::IsSafePathSegment(spec.version)) return false;
    if (!spec.files || spec.fileCount == 0) return false;

    bool hasEncoder = false;
    bool hasDecoder = false;
    bool hasJoiner = false;
    bool hasTokens = false;
    for (size_t i = 0; i < spec.fileCount; ++i) {
        const model_manager::ModelFileSpec& file = spec.files[i];
        if (!file.role || !file.relativePath || !file.sha256Hex) return false;
        if (!model_manager::IsKnownRole(file.role)) return false;
        if (!model_manager::IsSafeRelativePath(file.relativePath)) return false;
        model_manager::Sha256Digest probe{};
        if (!model_manager::ParseHexDigest(file.sha256Hex, &probe)) return false;
        hasEncoder = hasEncoder || SameName(file.role, kRoleEncoder);
        hasDecoder = hasDecoder || SameName(file.role, kRoleDecoder);
        hasJoiner = hasJoiner || SameName(file.role, kRoleJoiner);
        hasTokens = hasTokens || SameName(file.role, kRoleTokens);
    }
    // 四个角色缺一不可：缺任何一个都无法构造 recognizer 配置。
    return hasEncoder && hasDecoder && hasJoiner && hasTokens;
}

/// 单文件判定。*out 用于在成功时回填绝对路径。
SherpaModelStatus VerifyFile(const std::string& versionDir,
                             const model_manager::ModelFileSpec& file,
                             std::string* out) {    const std::string path = versionDir + "/" + file.relativePath;

    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) {
        return errno == ENOENT || errno == ENOTDIR
                   ? SherpaModelStatus::NotInstalled
                   : SherpaModelStatus::Unreadable;
    }
    // 只接受普通文件：符号链接、目录、FIFO 都不是受控内容
    // （安装器发布的是解包后的常规文件）。
    if (!S_ISREG(st.st_mode)) return SherpaModelStatus::Tampered;
    // 尺寸预检：不一致就不必哈希整个文件。
    if (static_cast<uint64_t>(st.st_size) != file.size) {
        return SherpaModelStatus::Tampered;
    }

    model_manager::Sha256Digest digest{};
    uint64_t hashedSize = 0;
    if (!model_manager::HashFileLimited(path, file.size, &digest, &hashedSize)) {
        // 打开失败（EACCES / O_NOFOLLOW 拒绝链接）或读错误。
        return SherpaModelStatus::Unreadable;
    }
    if (hashedSize != file.size) return SherpaModelStatus::Tampered;
    if (model_manager::ToHex(digest) != file.sha256Hex) {
        return SherpaModelStatus::Tampered;
    }

    if (out) *out = path;
    return SherpaModelStatus::Ok;
}

std::string* SlotForRole(const std::string& role, SherpaModelVerification* result) {
    if (role == kRoleEncoder) return &result->encoder;
    if (role == kRoleDecoder) return &result->decoder;
    if (role == kRoleJoiner) return &result->joiner;
    if (role == kRoleTokens) return &result->tokens;
    return nullptr;
}

/// 中间目录判定：必须是**真实目录**，符号链接一律视为重定向攻击。
/// 不存在按 NotInstalled（尚未安装），其它错误按 Unreadable。
SherpaModelStatus VerifyDirectoryComponent(const std::string& path) {
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) {
        return errno == ENOENT || errno == ENOTDIR
                   ? SherpaModelStatus::NotInstalled
                   : SherpaModelStatus::Unreadable;
    }
    if (S_ISLNK(st.st_mode)) return SherpaModelStatus::Tampered;
    if (!S_ISDIR(st.st_mode)) return SherpaModelStatus::Tampered;
    return SherpaModelStatus::Ok;
}

/// 去掉末尾 '/'，避免拼出 "root//id"。
std::string TrimTrailingSlashes(const std::string& path) {
    std::string out = path;
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

} // namespace

const char* SherpaModelStatusName(SherpaModelStatus status) {
    switch (status) {
    case SherpaModelStatus::Ok:
        return "ok";
    case SherpaModelStatus::NotInstalled:
        return "not_installed";
    case SherpaModelStatus::Tampered:
        return "tampered";
    case SherpaModelStatus::Unreadable:
        return "unreadable";
    case SherpaModelStatus::DataDirUnavailable:
        return "data_dir_unavailable";
    case SherpaModelStatus::InvalidSpec:
        return "invalid_spec";
    }
    return "unknown";
}

SherpaModelVerification VerifyModelVersionDir(
    const model_manager::ModelSpec& spec, const std::string& modelsRoot,
    const std::string& versionDir) {
    SherpaModelVerification result;

    if (!IsWellFormedSpec(spec) || modelsRoot.empty() || versionDir.empty()) {
        result.status = SherpaModelStatus::InvalidSpec;
        return result;
    }

    // 只允许验证「清单拼出来的那一条路径」：调用方不能把任意目录说成受控目录。
    const std::string root = TrimTrailingSlashes(modelsRoot);
    const std::string idDir = root + "/" + spec.id;
    const std::string expected = idDir + "/" + spec.version;
    if (expected != TrimTrailingSlashes(versionDir)) {
        result.status = SherpaModelStatus::InvalidSpec;
        return result;
    }

    // 受控链逐级检查：models → <id> → <version> 都必须是非链接目录。
    // 只在最终文件上做 O_NOFOLLOW 不足以阻止中间级重定向。
    for (const std::string& component : {root, idDir, expected}) {
        const SherpaModelStatus status = VerifyDirectoryComponent(component);
        if (status != SherpaModelStatus::Ok) {
            result.status = status;
            return result;
        }
    }

    // 按清单顺序校验（encoder → decoder → joiner → tokens）：
    // 大文件先失败时能更早返回，减少无谓 I/O。
    for (size_t i = 0; i < spec.fileCount; ++i) {
        const model_manager::ModelFileSpec& file = spec.files[i];
        std::string path;
        const SherpaModelStatus status = VerifyFile(versionDir, file, &path);
        if (status != SherpaModelStatus::Ok) {
            result.status = status;
            result.failedRelativePath = file.relativePath;
            result.encoder.clear();
            result.decoder.clear();
            result.joiner.clear();
            result.tokens.clear();
            return result;
        }
        if (std::string* slot = SlotForRole(file.role, &result)) {
            *slot = std::move(path);
        }
    }

    result.status = SherpaModelStatus::Ok;
    return result;
}

} // namespace fcitx
