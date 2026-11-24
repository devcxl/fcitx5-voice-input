#include "fs_safe.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace fcitx::model_manager {

namespace {

// renameat2 的 flags（避免依赖内核头文件的具体版本）。
constexpr unsigned kRenameNoReplace = 1U << 0; // RENAME_NOREPLACE

constexpr size_t kCopyChunk = 256 * 1024;

/// 只保留文件名安全的字符，避免 label 逃出暂存根目录。
std::string SanitizeLabel(const std::string& label) {
    std::string out;
    out.reserve(label.size());
    for (char c : label) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                          c == '-' || c == '_';
        if (keep) {
            out.push_back(c);
        } else if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        }
    }
    if (out.empty()) out = "model";
    if (out.size() > 64) out.resize(64);
    return out;
}

} // namespace

bool EnsureDir(const std::string& path, unsigned mode, std::string* err) {
    if (path.empty()) {
        if (err) *err = "目录路径为空";
        return false;
    }

    // 逐级创建（与 create_directories 同语义），但只在**本次创建**的那一级
    // 设置权限：绝不对已存在的目录 chmod。
    std::string current;
    size_t index = 0;
    if (path.front() == '/') {
        current = "/";
        index = 1;
    }

    while (index <= path.size()) {
        const size_t slash = path.find('/', index);
        const size_t end = slash == std::string::npos ? path.size() : slash;
        const std::string component = path.substr(index, end - index);
        index = end + 1;

        if (component.empty()) {
            if (end == path.size()) break;
            continue;
        }

        if (!current.empty() && current.back() != '/') current.push_back('/');
        current += component;

        if (::mkdir(current.c_str(), static_cast<mode_t>(mode)) == 0) {
            continue; // 本次创建
        }
        if (errno != EEXIST) {
            if (err) {
                *err = "无法创建目录 " + current + ": " + std::strerror(errno);
            }
            return false;
        }
        // 已存在：必须是真实目录（符号链接不接受），且不改其权限。
        struct stat st {};
        if (::lstat(current.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            if (err) *err = "路径已存在但不是目录: " + current;
            return false;
        }
    }
    return true;
}

bool EnsureDirUnder(const std::string& path, const std::string& trustedBase,
                    unsigned mode, std::string* err) {
    if (path.empty()) {
        if (err) *err = "目录路径为空";
        return false;
    }
    if (trustedBase.empty()) return EnsureDir(path, mode, err);

    // trustedBase 及其上层：按普通路径创建/跟随（允许符号链接，用户 HOME/XDG 常见）。
    std::error_code ec;
    const bool created = std::filesystem::create_directories(trustedBase, ec);
    if (ec) {
        if (err) *err = "无法创建受控根目录 " + trustedBase + ": " + ec.message();
        return false;
    }
    if (created) ::chmod(trustedBase.c_str(), static_cast<mode_t>(mode));

    struct stat baseSt {};
    if (::stat(trustedBase.c_str(), &baseSt) != 0 || !S_ISDIR(baseSt.st_mode)) {
        if (err) *err = "受控根路径不是目录: " + trustedBase;
        return false;
    }

    if (path == trustedBase) return true;
    if (path.size() <= trustedBase.size() ||
        path.compare(0, trustedBase.size(), trustedBase) != 0 ||
        path[trustedBase.size()] != '/') {
        if (err) *err = "路径不在受控根之下: " + path;
        return false;
    }

    // 受控段：**从 trustedBase 之下**逐级创建并检查，不再回看上层，
    // 因此上层的符号链接不会误伤；这一段内出现符号链接则拒绝。
    std::string current = trustedBase;
    const std::string relative = path.substr(trustedBase.size() + 1);
    size_t index = 0;
    while (index <= relative.size()) {
        const size_t slash = relative.find('/', index);
        const size_t end = slash == std::string::npos ? relative.size() : slash;
        const std::string component = relative.substr(index, end - index);
        index = end + 1;
        if (component.empty()) {
            if (end == relative.size()) break;
            if (err) *err = "路径包含空组件: " + path;
            return false;
        }
        if (component == "." || component == "..") {
            if (err) *err = "路径包含非法组件: " + component;
            return false;
        }

        if (current.back() != '/') current.push_back('/');
        current += component;

        if (::mkdir(current.c_str(), static_cast<mode_t>(mode)) == 0) continue;
        if (errno != EEXIST) {
            if (err) {
                *err = "无法创建目录 " + current + ": " + std::strerror(errno);
            }
            return false;
        }
        // 已存在：受控段内必须是真实目录（符号链接会逃出受控范围）。
        struct stat st {};
        if (::lstat(current.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            if (err) *err = "受控路径已存在但不是真实目录: " + current;
            return false;
        }
    }
    return true;
}

std::string MakeUniqueDir(const std::string& root, const std::string& label,
                          std::string* err) {
    if (root.empty()) {
        if (err) *err = "暂存根目录为空";
        return {};
    }
    std::string tmpl = root + "/" + SanitizeLabel(label) + "-XXXXXX";
    std::vector<char> buffer(tmpl.begin(), tmpl.end());
    buffer.push_back('\0');
    // mkdtemp 以独占方式创建 0700 目录：不可能复用已存在的目录。
    if (!::mkdtemp(buffer.data())) {
        if (err) {
            *err = std::string("无法创建暂存目录: ") + std::strerror(errno);
        }
        return {};
    }
    return std::string(buffer.data());
}

const char* CopyStatusName(CopyStatus status) {
    switch (status) {
        case CopyStatus::Ok: return "ok";
        case CopyStatus::Cancelled: return "cancelled";
        case CopyStatus::SourceNotRegular: return "source_not_regular";
        case CopyStatus::SourceTooLarge: return "source_too_large";
        case CopyStatus::IoError: return "io_error";
    }
    return "io_error";
}

CopyStatus CopyFileBounded(const std::string& src, const std::string& dst,
                           uint64_t maxBytes,
                           const std::function<bool()>& cancelled,
                           std::string* err) {
    if (src.empty() || dst.empty()) {
        if (err) *err = "复制路径为空";
        return CopyStatus::IoError;
    }

    // 源文件：不跟随链接、不阻塞；类型与大小在 fd 上判定（TOCTOU 安全）。
    const int in = ::open(src.c_str(),
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (in < 0) {
        // ELOOP = 路径是符号链接且被 O_NOFOLLOW 拒绝：这是"类型不对"，
        // 与被拒绝的 FIFO/设备同属一类，便于调用方给出准确提示。
        const int openErrno = errno;
        if (err) {
            *err = std::string("无法打开源文件: ") +
                   std::strerror(openErrno);
        }
        return openErrno == ELOOP ? CopyStatus::SourceNotRegular
                                  : CopyStatus::IoError;
    }
    struct stat st {};
    if (::fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(in);
        if (err) *err = "源路径不是普通文件";
        return CopyStatus::SourceNotRegular;
    }
    if (maxBytes > 0 && static_cast<uint64_t>(st.st_size) > maxBytes) {
        ::close(in);
        if (err) *err = "源文件大于允许的上限";
        return CopyStatus::SourceTooLarge;
    }

    // 目标：独占创建，绝不覆盖已有文件。
    const int out = ::open(dst.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                           0600);
    if (out < 0) {
        ::close(in);
        if (err) *err = std::string("无法创建目标文件: ") + std::strerror(errno);
        return CopyStatus::IoError;
    }

    std::vector<char> buffer(kCopyChunk);
    uint64_t total = 0;
    CopyStatus status = CopyStatus::Ok;
    while (status == CopyStatus::Ok) {
        if (cancelled && cancelled()) {
            if (err) *err = "复制已取消";
            status = CopyStatus::Cancelled;
            break;
        }
        const ssize_t n = ::read(in, buffer.data(), buffer.size());
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            if (err) *err = std::string("读取源文件失败: ") + std::strerror(errno);
            status = CopyStatus::IoError;
            break;
        }
        total += static_cast<uint64_t>(n);
        if (maxBytes > 0 && total > maxBytes) {
            if (err) *err = "源文件超过允许的上限";
            status = CopyStatus::SourceTooLarge;
            break;
        }
        const char* p = buffer.data();
        size_t written = 0;
        while (written < static_cast<size_t>(n)) {
            const ssize_t w = ::write(out, p + written,
                                      static_cast<size_t>(n) - written);
            if (w < 0) {
                if (errno == EINTR) continue;
                if (err) {
                    *err = std::string("写入目标文件失败: ") +
                           std::strerror(errno);
                }
                status = CopyStatus::IoError;
                break;
            }
            written += static_cast<size_t>(w);
        }
    }

    if (status == CopyStatus::Ok && ::fsync(out) != 0) {
        if (err) *err = std::string("刷盘失败: ") + std::strerror(errno);
        status = CopyStatus::IoError;
    }
    ::close(out);
    ::close(in);
    return status;
}

const char* ChainStatusName(ChainStatus status) {
    switch (status) {
        case ChainStatus::Ok: return "ok";
        case ChainStatus::Missing: return "missing";
        case ChainStatus::Symlink: return "symlink";
        case ChainStatus::NotDirectory: return "not_directory";
        case ChainStatus::IoError: return "io_error";
    }
    return "io_error";
}

ChainStatus CheckDirChain(const std::string& base, const std::string& relative,
                          std::string* badComponent) {
    if (badComponent) badComponent->clear();
    if (base.empty() || relative.empty()) return ChainStatus::IoError;

    std::string current = base;
    size_t index = 0;
    while (index < relative.size()) {
        const size_t slash = relative.find('/', index);
        const size_t end = slash == std::string::npos ? relative.size() : slash;
        const std::string component = relative.substr(index, end - index);
        index = end + 1;
        if (component.empty() || component == "." || component == "..") {
            if (badComponent) *badComponent = component;
            return ChainStatus::IoError;
        }

        if (current.back() != '/') current.push_back('/');
        current += component;

        struct stat st {};
        if (::lstat(current.c_str(), &st) != 0) {
            const int callErrno = errno;
            if (badComponent) *badComponent = current;
            if (callErrno == ENOENT) return ChainStatus::Missing;
            return ChainStatus::IoError;
        }
        if (S_ISLNK(st.st_mode)) {
            if (badComponent) *badComponent = current;
            return ChainStatus::Symlink;
        }
        if (!S_ISDIR(st.st_mode)) {
            if (badComponent) *badComponent = current;
            return ChainStatus::NotDirectory;
        }
    }
    return ChainStatus::Ok;
}

bool FsyncDir(const std::string& path) {
    if (path.empty()) return false;
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

OpenDirChainResult OpenDirChainNoFollow(const std::string& trustedBase,
                                        const std::string& relative) {
    OpenDirChainResult result;

    if (trustedBase.empty()) {
        result.status = ChainStatus::IoError;
        return result;
    }

    // 信任边界自身：允许符号链接（用户 HOME/XDG 常如此），用普通 open。
    int fd = ::open(trustedBase.c_str(),
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        result.status = errno == ENOENT ? ChainStatus::Missing
                                        : ChainStatus::IoError;
        result.badComponent = trustedBase;
        return result;
    }

    std::string current = trustedBase;
    size_t index = 0;
    while (index < relative.size()) {
        const size_t slash = relative.find('/', index);
        const size_t end = slash == std::string::npos ? relative.size() : slash;
        const std::string component = relative.substr(index, end - index);
        index = end + 1;
        if (component.empty() || component == "." || component == "..") {
            ::close(fd);
            result.status = ChainStatus::IoError;
            result.badComponent = component;
            return result;
        }

        current += "/" + component;
        // O_NOFOLLOW + O_DIRECTORY：该级是符号链接或不是目录时直接失败，
        // 因此 rename 不可能被导向受控目录之外。
        const int next = ::openat(fd, component.c_str(),
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        const int openErrno = errno;
        ::close(fd);
        if (next < 0) {
            result.status = openErrno == ENOENT
                                ? ChainStatus::Missing
                                : (openErrno == ELOOP || openErrno == ENOTDIR
                                       ? (openErrno == ELOOP ? ChainStatus::Symlink
                                                            : ChainStatus::NotDirectory)
                                       : ChainStatus::IoError);
            result.badComponent = current;
            return result;
        }
        fd = next;
    }

    result.fd = fd;
    result.status = ChainStatus::Ok;
    return result;
}

const char* NoReplaceStatusName(NoReplaceStatus status) {
    switch (status) {
        case NoReplaceStatus::Ok: return "ok";
        case NoReplaceStatus::Exists: return "exists";
        case NoReplaceStatus::Unsupported: return "unsupported";
        case NoReplaceStatus::IoError: return "io_error";
    }
    return "io_error";
}

NoReplaceStatus RenameNoReplaceAt(int srcDirFd, const std::string& srcName,
                                  int dstDirFd, const std::string& dstName,
                                  std::string* err) {
#if defined(__linux__) && defined(SYS_renameat2)
    auto badName = [](const std::string& name) {
        return name.empty() || name.find('/') != std::string::npos ||
               name == "." || name == "..";
    };
    if (srcDirFd < 0 || dstDirFd < 0 || badName(srcName) || badName(dstName)) {
        if (err) *err = "RenameNoReplaceAt 参数无效";
        return NoReplaceStatus::IoError;
    }
    if (::syscall(SYS_renameat2, srcDirFd, srcName.c_str(), dstDirFd,
                  dstName.c_str(), kRenameNoReplace) == 0) {
        return NoReplaceStatus::Ok;
    }
    const int callErrno = errno;
    if (err) {
        *err = std::string("renameat2(RENAME_NOREPLACE) 失败: ") +
               std::strerror(callErrno);
    }
    if (callErrno == EEXIST) return NoReplaceStatus::Exists;
    if (callErrno == EINVAL || callErrno == ENOSYS) {
        return NoReplaceStatus::Unsupported;
    }
    return NoReplaceStatus::IoError;
#else
    (void)srcDirFd;
    (void)srcName;
    (void)dstDirFd;
    (void)dstName;
    if (err) *err = "本平台不支持 RENAME_NOREPLACE";
    return NoReplaceStatus::Unsupported;
#endif
}

} // namespace fcitx::model_manager
