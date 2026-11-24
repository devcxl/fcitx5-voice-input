#include "safe_remove.h"

#include <cerrno>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs_safe.h"

namespace fcitx::model_manager {

namespace {

constexpr int kMaxRemoveDepth = 64;

/// 递归删除 dirFd 下的条目；目录条目用 `O_NOFOLLOW` 进入，符号链接只 unlink。
bool RemoveDirContentsAtDepth(int dirFd, int depth, std::string* err) {
    if (depth > kMaxRemoveDepth) {
        if (err) *err = "目录层级过深，已中止删除";
        return false;
    }

    const int scanFd = ::dup(dirFd);
    if (scanFd < 0) {
        if (err) *err = std::string("dup 失败: ") + std::strerror(errno);
        return false;
    }
    DIR* dir = ::fdopendir(scanFd);
    if (!dir) {
        ::close(scanFd);
        if (err) *err = std::string("fdopendir 失败: ") + std::strerror(errno);
        return false;
    }

    bool ok = true;
    errno = 0;
    while (struct dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        struct stat st {};
        if (::fstatat(dirFd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) continue;
            if (err) {
                *err = "无法读取条目 " + name + ": " + std::strerror(errno);
            }
            ok = false;
            break;
        }

        if (S_ISDIR(st.st_mode)) {
            const int child = ::openat(dirFd, name.c_str(),
                                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                           O_CLOEXEC);
            if (child < 0) {
                if (errno == ELOOP) {
                    // 实际是符号链接：只 unlink，不跟随。
                    if (::unlinkat(dirFd, name.c_str(), 0) != 0) {
                        if (err) {
                            *err = "无法删除链接 " + name + ": " +
                                   std::strerror(errno);
                        }
                        ok = false;
                        break;
                    }
                    continue;
                }
                if (err) {
                    *err = "无法进入目录 " + name + ": " +
                           std::strerror(errno);
                }
                ok = false;
                break;
            }
            ok = RemoveDirContentsAtDepth(child, depth + 1, err);
            ::close(child);
            if (!ok) break;
            if (::unlinkat(dirFd, name.c_str(), AT_REMOVEDIR) != 0) {
                if (err) {
                    *err = "无法移除目录 " + name + ": " +
                           std::strerror(errno);
                }
                ok = false;
                break;
            }
        } else {
            if (::unlinkat(dirFd, name.c_str(), 0) != 0) {
                if (err) {
                    *err = "无法删除条目 " + name + ": " +
                           std::strerror(errno);
                }
                ok = false;
                break;
            }
        }
    }
    ::closedir(dir); // 同时关闭 scanFd
    return ok;
}

} // namespace

DirIdentity DirIdentityOfFd(int fd) {
    DirIdentity identity;
    if (fd < 0) return identity;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) return identity;
    identity.dev = st.st_dev;
    identity.ino = st.st_ino;
    identity.valid = true;
    return identity;
}

PinnedDir::~PinnedDir() {
    if (fd_ >= 0) ::close(fd_);
}

PinnedDir::PinnedDir(PinnedDir&& other) noexcept
    : fd_(other.fd_), identity_(other.identity_) {
    other.fd_ = -1;
    other.identity_ = DirIdentity{};
}

PinnedDir& PinnedDir::operator=(PinnedDir&& other) noexcept {
    if (this == &other) return *this;
    if (fd_ >= 0) ::close(fd_);
    fd_ = other.fd_;
    identity_ = other.identity_;
    other.fd_ = -1;
    other.identity_ = DirIdentity{};
    return *this;
}

int PinnedDir::Release() {
    const int fd = fd_;
    fd_ = -1;
    identity_ = DirIdentity{};
    return fd;
}

PinnedDir PinnedDir::Open(const std::string& path, std::string* err) {
    PinnedDir pinned;
    if (path.empty()) {
        if (err) *err = "路径为空";
        return pinned;
    }
    const int fd = ::open(path.c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (err) *err = std::string("无法打开目录: ") + std::strerror(errno);
        return pinned;
    }
    const DirIdentity identity = DirIdentityOfFd(fd);
    if (!identity.valid) {
        ::close(fd);
        if (err) *err = "路径不是目录";
        return pinned;
    }
    pinned.fd_ = fd;
    pinned.identity_ = identity;
    return pinned;
}

DirIdentity DirIdentityOfPath(const std::string& path, std::string* err) {
    DirIdentity identity;
    if (path.empty()) {
        if (err) *err = "路径为空";
        return identity;
    }
    const int fd = ::open(path.c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (err) *err = std::string("无法打开目录: ") + std::strerror(errno);
        return identity;
    }
    identity = DirIdentityOfFd(fd);
    ::close(fd);
    if (!identity.valid && err) *err = "路径不是目录或是符号链接";
    return identity;
}

bool RemoveDirContentsAt(int dirFd, std::string* err) {
    if (dirFd < 0) {
        if (err) *err = "目录 fd 无效";
        return false;
    }
    return RemoveDirContentsAtDepth(dirFd, 0, err);
}

bool RemoveTreePinned(const std::string& trustedBase,
                      const std::string& relativePath, int pinnedFd,
                      std::string* err) {
    if (relativePath.empty() || trustedBase.empty()) {
        if (err) *err = "删除路径为空";
        return false;
    }
    if (pinnedFd < 0) {
        // 没有钉住对象就无法可靠判断"是否同一个目录"：宁可保留。
        if (err) *err = "缺少创建时钉住的目录 fd，已保留（保守策略）";
        return false;
    }

    const DirIdentity pinnedIdentity = DirIdentityOfFd(pinnedFd);
    if (!pinnedIdentity.valid) {
        if (err) *err = "钉住 fd 已不可用，已保留（保守策略）";
        return false;
    }

    const size_t slash = relativePath.find_last_of('/');
    if (slash == std::string::npos || slash == 0 ||
        slash + 1 >= relativePath.size()) {
        if (err) *err = "删除路径必须是 <父相对路径>/<末级名>";
        return false;
    }
    const std::string parentRelative = relativePath.substr(0, slash);
    const std::string leaf = relativePath.substr(slash + 1);

    const OpenDirChainResult parent =
        OpenDirChainNoFollow(trustedBase, parentRelative);
    if (parent.fd < 0) {
        if (err) {
            *err = std::string("无法安全打开待删目录的父目录（") +
                   ChainStatusName(parent.status) + "）: " + parent.badComponent;
        }
        return false;
    }

    const int target = ::openat(parent.fd, leaf.c_str(),
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (target < 0) {
        const int callErrno = errno;
        ::close(parent.fd);
        if (err) {
            *err = std::string("无法安全打开待删目录: ") +
                   std::strerror(callErrno);
        }
        return false;
    }

    const DirIdentity actual = DirIdentityOfFd(target);
    // 钉住 fd 让该 inode 保持存活，因此"不相等"确实意味着这是另一个目录，
    // 不会因为 inode 被回收复用而误判。
    if (!actual.valid || actual.dev != pinnedIdentity.dev ||
        actual.ino != pinnedIdentity.ino) {
        ::close(target);
        ::close(parent.fd);
        if (err) *err = "待删目录不是本次创建的那个目录，已保留（未被本次删除）";
        return false;
    }

    const bool cleared = RemoveDirContentsAt(target, err);
    ::close(target);
    if (!cleared) {
        ::close(parent.fd);
        return false;
    }

    if (::unlinkat(parent.fd, leaf.c_str(), AT_REMOVEDIR) != 0) {
        const int callErrno = errno;
        ::close(parent.fd);
        if (err) {
            *err = std::string("移除目录失败: ") + std::strerror(callErrno);
        }
        return false;
    }
    ::close(parent.fd);
    return true;
}

} // namespace fcitx::model_manager
