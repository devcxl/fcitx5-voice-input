#pragma once

// 安全删除：按"可信父目录 fd + 相对路径 + 创建时身份"定位目标，不跟随符号链接，
// 也不按路径递归。
//
// **信任边界**：同 UID 且可写该目录树的进程可与本模块竞争（在身份采集与删除
// 之间替换条目）。本模块的检查是尽力而为的一致性校验，用于应对并发安装、
// 目录被搬走、异常清理等非对抗场景，**不构成**对主动恶意同 UID 进程的防御。
//
// 与 fs_safe.h 的分工：fs_safe 负责创建/复制/定位；本模块负责删除与身份校验。

#include <string>

#include <sys/types.h>

namespace fcitx::model_manager {

/// 目录身份（设备号 + inode）。
///
/// **只在持有该目录的打开 fd 时才有比较意义**：目录被删除后 inode 会被文件系统
/// 回收并复用，单凭"历史 dev/ino 值"无法区分原目录与复用同一 inode 的新目录。
/// 需要跨操作保持身份时，请使用 `PinnedDir`（持有 fd，阻止 inode 回收）。
struct DirIdentity {
    dev_t dev = 0;
    ino_t ino = 0;
    bool valid = false;
};

/// 钉住的目录：从创建那一刻起持有 fd，直到对象销毁。
///
/// 关键性质：只要 fd 打开，该 inode 就不会被回收、也不会被新目录复用，
/// 因此后续"目标 fd 的 dev/ino 是否等于钉住对象的 dev/ino"是**可靠**判断，
/// 而不是对已释放 inode 号的历史比较。
///
/// 生命周期：必须比使用它的清理/发布操作活得更久。析构顺序按声明逆序，
/// 因此"钉住对象"应先声明、"清理守卫"后声明。
class PinnedDir {
public:
    PinnedDir() = default;
    ~PinnedDir();

    PinnedDir(const PinnedDir&) = delete;
    PinnedDir& operator=(const PinnedDir&) = delete;
    PinnedDir(PinnedDir&& other) noexcept;
    PinnedDir& operator=(PinnedDir&& other) noexcept;

    /// 打开 path（`O_NOFOLLOW|O_DIRECTORY`，拒绝符号链接）并钉住。
    /// 失败返回无效对象并写 err。
    static PinnedDir Open(const std::string& path, std::string* err);

    bool valid() const { return fd_ >= 0; }
    int fd() const { return fd_; }
    /// 从 fd 读取的身份（创建后立即固定，之后可直接比较）。
    const DirIdentity& identity() const { return identity_; }

    /// 放弃所有权并返回 fd（调用方负责关闭）。
    int Release();

private:
    int fd_ = -1;
    DirIdentity identity_;
};

/// 读取路径当前对应的目录身份（`O_NOFOLLOW|O_DIRECTORY` 打开后 fstat）。
/// 路径是符号链接或不是目录时返回 valid=false。
DirIdentity DirIdentityOfPath(const std::string& path, std::string* err);

/// 从已打开的目录 fd 读取身份。
DirIdentity DirIdentityOfFd(int fd);

/// 递归删除**已打开**目录 fd 的全部内容（不含该目录自身）。
///
/// 全部操作基于 `fdopendir` + `openat(O_NOFOLLOW)` + `unlinkat`：
///   - 符号链接只被 unlink，不会被跟随；
///   - 不拼接路径，因此不受中间级路径被替换的影响；
///   - 深度有上限，防止异常结构导致栈溢出。
bool RemoveDirContentsAt(int dirFd, std::string* err);

/// 按"可信父目录 + 相对路径 + 期望身份"删除目录树。
///
/// 语义：
///   - 从 `trustedBase` 逐级 `openat(O_DIRECTORY|O_NOFOLLOW)` 定位目标；
///   - 打开目标后 `fstat` 比对 `expected`（dev+ino）：不一致（说明被替换）或
///     无法确定身份时**保守保留**，返回 false 且 err 说明原因；
///   - 身份一致时只删除该 fd 下的内容，并用 `unlinkat(AT_REMOVEDIR)` 移除该级。
///
/// 因此在常见竞态下（`.staging` 被换成符号链接、同名目录被重建）会保留而非
/// 误删。仍然存在的残余风险见头注释的信任边界：同 UID 可在校验与删除之间
/// 替换条目，此时删除作用于**调用时存在于该目录内**的条目。
///   - 目标 fd 的 dev/ino 必须与 `pinned`（创建时钉住、仍打开的 fd）一致；
///     因为钉住对象始终存活，这个比较不会受 inode 复用影响。
///
/// `pinned` 是创建该目录时得到的打开 fd；调用方必须保证它在本函数返回前有效。
bool RemoveTreePinned(const std::string& trustedBase,
                      const std::string& relativePath, int pinnedFd,
                      std::string* err);

} // namespace fcitx::model_manager
