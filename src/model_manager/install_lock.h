#pragma once

// 安装互斥锁：同一模型的并发安装必须串行化或幂等收敛。
//
// 用 flock(2) 而非 PID 文件：
//   - 进程崩溃/被 SIGKILL 时内核自动释放，不会留下"僵尸锁"；
//   - 锁随文件描述符存在，exec 时由 O_CLOEXEC 断开，子进程不会误持锁。
//
// 锁文件位于 <root>/.locks/<id>.lock，锁内容无关紧要（只用作互斥载体）。

#include <string>

namespace fcitx::model_manager {

enum class LockStatus {
    Acquired = 0,
    Busy,       // 另一个进程持有锁
    Failed,     // 无法创建/打开锁文件（权限、磁盘、路径）
};

const char* LockStatusName(LockStatus status);

/// RAII 锁。析构即解锁。
class InstallLock {
public:
    InstallLock() = default;
    ~InstallLock();
    InstallLock(const InstallLock&) = delete;
    InstallLock& operator=(const InstallLock&) = delete;

    /// 以非阻塞方式尝试取锁。成功时持有到析构。
    LockStatus TryAcquire(const std::string& lockPath, std::string* detail);

    bool held() const { return fd_ >= 0; }

    /// 主动释放（析构也会做）。
    void Release();

private:
    int fd_ = -1;
};

} // namespace fcitx::model_manager
