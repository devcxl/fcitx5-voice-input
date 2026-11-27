#include "install_lock.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace fcitx::model_manager {

const char* LockStatusName(LockStatus status) {
    switch (status) {
        case LockStatus::Acquired: return "acquired";
        case LockStatus::Busy: return "busy";
        case LockStatus::Failed: return "failed";
    }
    return "failed";
}

InstallLock::~InstallLock() { Release(); }

LockStatus InstallLock::TryAcquire(const std::string& lockPath,
                                   std::string* detail) {
    Release();
    if (lockPath.empty()) {
        if (detail) *detail = "锁路径为空";
        return LockStatus::Failed;
    }

    // O_NOFOLLOW：锁文件不应是符号链接，避免被引到别处。
    const int fd = ::open(lockPath.c_str(),
                          O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        if (detail) {
            *detail = std::string("无法打开锁文件: ") + std::strerror(errno);
        }
        return LockStatus::Failed;
    }

    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int err = errno;
        ::close(fd);
        if (err == EWOULDBLOCK) return LockStatus::Busy;
        if (detail) {
            *detail = std::string("flock 失败: ") + std::strerror(err);
        }
        return LockStatus::Failed;
    }

    fd_ = fd;
    return LockStatus::Acquired;
}

void InstallLock::Release() {
    if (fd_ < 0) return;
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
}

} // namespace fcitx::model_manager
