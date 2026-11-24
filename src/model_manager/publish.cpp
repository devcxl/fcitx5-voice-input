#include "publish.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs_safe.h"
#include "safe_remove.h"
#include "installed_model.h"
#include "model_paths.h"

namespace fcitx::model_manager {

namespace {

/// 关闭 fd 的 RAII（避免多处提前 return 泄漏）。
struct FdCloser {
    int fd;
    ~FdCloser() {
        if (fd >= 0) ::close(fd);
    }
};

PublishOutcome Fail(InstallError error, std::string detail) {
    PublishOutcome out;
    out.ok = false;
    out.error = error;
    out.detail = std::move(detail);
    return out;
}

/// 取 finalDir 的父目录与末级名字（都是单级安全片段）。
bool SplitParent(const std::string& path, std::string* parent, std::string* leaf) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= path.size()) {
        return false;
    }
    *parent = path.substr(0, slash);
    *leaf = path.substr(slash + 1);
    if (leaf->empty() || leaf->find('/') != std::string::npos ||
        *leaf == "." || *leaf == "..") {
        return false;
    }
    return true;
}

} // namespace

const char* PublishedPathStateName(PublishedPathState state) {
    switch (state) {
        case PublishedPathState::Ok: return "ok";
        case PublishedPathState::Uncertain: return "uncertain";
    }
    return "uncertain";
}

PublishedPathState CheckPublishedPath(int parentFd, const std::string& leaf,
                                     const std::string& finalDir) {
    if (parentFd < 0 || leaf.empty() || finalDir.empty()) {
        return PublishedPathState::Uncertain;
    }

    // 1) 已钉住父目录下的那个条目（不跟随符号链接）。
    struct stat entrySt {};
    if (::fstatat(parentFd, leaf.c_str(), &entrySt, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISDIR(entrySt.st_mode)) {
        return PublishedPathState::Uncertain;
    }

    // 2) 按路径解析的结果（O_NOFOLLOW，符号链接视为不一致）。
    std::string err;
    const DirIdentity byPath = DirIdentityOfPath(finalDir, &err);
    if (!byPath.valid) return PublishedPathState::Uncertain;

    return (byPath.dev == entrySt.st_dev && byPath.ino == entrySt.st_ino)
               ? PublishedPathState::Ok
               : PublishedPathState::Uncertain;
}

PublishIo DefaultPublishIo() {
    PublishIo io;
    io.fsyncFd = [](int fd) { return fd >= 0 && ::fsync(fd) == 0; };
    return io;
}

PublishOutcome PublishVersionDir(const ModelSpec& spec,
                                 const PublishSource& source,
                                 const std::string& finalDir,
                                 bool finalExistedUpFront,
                                 const std::function<bool()>& cancelled,
                                 const PublishIo& io) {
    (void)spec; // 发布只依赖路径；保留参数以便将来按 spec 做额外校验。
    PublishOutcome out;

    const PublishIo real = DefaultPublishIo();
    const PublishIo& ops = io.fsyncFd ? io : real;

    // 1) 用 fd 钉住源目录，先复核身份，再刷盘。
    //
    //    只按路径 rename 是不够的：源父级若被搬走/替换，可能把别的目录移进模型
    //    路径。这里从受控根逐级 openat(O_NOFOLLOW) 打开源目录，fstat 与创建时
    //    记录的身份比对；不一致或身份未知一律拒绝发布。
    if (source.relativeToModelsParent.empty() || source.pinnedFd < 0) {
        return Fail(InstallError::PublishFailed,
                    "解包目录缺少创建时钉住的 fd，拒绝发布");
    }
    const std::string trustedBase = ModelsParentDir();
    if (trustedBase.empty() ||
        source.relativeToModelsParent.find("..") != std::string::npos) {
        return Fail(InstallError::PublishFailed, "解包目录路径不在受控根之下");
    }

    const size_t srcSlash =
        source.relativeToModelsParent.find_last_of('/');
    if (srcSlash == std::string::npos || srcSlash == 0 ||
        srcSlash + 1 >= source.relativeToModelsParent.size()) {
        return Fail(InstallError::PublishFailed, "解包目录相对路径非法");
    }
    const std::string srcParentRelative =
        source.relativeToModelsParent.substr(0, srcSlash);
    const std::string srcLeaf =
        source.relativeToModelsParent.substr(srcSlash + 1);

    const OpenDirChainResult srcParent =
        OpenDirChainNoFollow(trustedBase, srcParentRelative);
    if (srcParent.fd < 0) {
        return Fail(InstallError::PublishFailed,
                    std::string("无法安全打开解包目录的父目录（") +
                        ChainStatusName(srcParent.status) + "）: " +
                        srcParent.badComponent);
    }
    const FdCloser srcParentCloser{srcParent.fd};

    const int srcFd =
        ::openat(srcParent.fd, srcLeaf.c_str(),
                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (srcFd < 0) {
        return Fail(InstallError::PublishFailed,
                    "无法安全打开解包目录以刷盘: " + source.dir);
    }
    const FdCloser srcCloser{srcFd};

    // 与**钉住的 fd** 比对：该 fd 由调用方保持打开，故 inode 不会被回收复用，
    // 比较结果可靠（历史 dev/ino 值做不到这一点）。
    const DirIdentity pinnedIdentity = DirIdentityOfFd(source.pinnedFd);
    const DirIdentity srcIdentity = DirIdentityOfFd(srcFd);
    if (!pinnedIdentity.valid || !srcIdentity.valid ||
        srcIdentity.dev != pinnedIdentity.dev ||
        srcIdentity.ino != pinnedIdentity.ino) {
        // 源目录已被替换：内容不再是我们校验过的那份，拒绝发布。
        return Fail(InstallError::PublishFailed,
                    "解包目录身份与校验时不一致，拒绝发布（可能被外部替换）");
    }

    const bool contentSynced = ops.fsyncFd(srcFd);
    if (!contentSynced) {
        return Fail(InstallError::PublishFailed,
                    "解包内容刷盘失败，已中止发布（版本目录未创建）");
    }

    // 2) 廉价早退：fsync 之后先看一眼取消。
    if (cancelled && cancelled()) {
        return Fail(InstallError::Cancelled, "安装已取消（发布前，未写入版本目录）");
    }

    // 3) 用目录 fd 钉住父目录，再做 rename。
    //
    //    仅做 lstat/路径校验是不够的：校验之后父路径可能被换成符号链接，裸
    //    rename 会把内容写到受控目录之外。这里从信任边界（models 的父目录）
    //    逐级 openat(O_DIRECTORY|O_NOFOLLOW) 拿到父目录 fd，再 renameat2 到
    //    该 fd 下，符号链接在打开时就会被拒绝。
    //    注意：这消除了"父目录被换成符号链接"这一窗口，但**不**声称消除所有
    //    路径竞态（例如把某级替换成另一个真实目录）。
    std::string parentDir;
    std::string leaf;
    if (!SplitParent(finalDir, &parentDir, &leaf)) {
        return Fail(InstallError::PublishFailed,
                    "版本目录路径非法: " + finalDir);
    }

    if (trustedBase.empty() || parentDir.rfind(trustedBase + "/", 0) != 0) {
        return Fail(InstallError::PublishFailed,
                    "版本目录不在受控根之下，拒绝发布: " + finalDir);
    }
    const std::string parentRelative = parentDir.substr(trustedBase.size() + 1);

    const OpenDirChainResult opened =
        OpenDirChainNoFollow(trustedBase, parentRelative);
    if (opened.fd < 0) {
        return Fail(InstallError::PublishFailed,
                    std::string("无法安全打开目标父目录（") +
                        ChainStatusName(opened.status) + "）: " +
                        opened.badComponent);
    }
    // 从 fd 路径释放责任：用 RAII 关闭，避免提前 return 泄漏。
    const FdCloser destCloser{opened.fd};

    // 3) **最终**取消检查：紧邻 rename。
    //
    //    OpenDirChainNoFollow 会做多次 openat，在慢/挂起的文件系统上可能阻塞；
    //    只有在拿到 fd 之后再检查，才能覆盖这段耗时。此前的检查保留作廉价早退。
    if (cancelled && cancelled()) {
        return Fail(InstallError::Cancelled, "安装已取消（发布前，未写入版本目录）");
    }

    // 4) 唯一允许的发布原语：目标不存在才成功。
    std::string noReplaceErr;
    const NoReplaceStatus placed = RenameNoReplaceAt(
        srcParent.fd, srcLeaf, opened.fd, leaf, &noReplaceErr);

    if (placed == NoReplaceStatus::Exists) {
        if (!finalExistedUpFront) {
            return Fail(InstallError::PublishFailed,
                        "目标版本目录在安装期间被外部创建，已拒绝覆盖: " +
                            finalDir);
        }
        return Fail(InstallError::PublishFailed,
                    "已存在未通过校验的版本目录，拒绝自动替换；"
                    "请确认后手动移除再重试: " +
                        finalDir);
    }

    if (placed == NoReplaceStatus::Unsupported) {
        return Fail(InstallError::PublishFailed,
                    "文件系统不支持 RENAME_NOREPLACE，无法安全发布: " +
                        finalDir + "（" + noReplaceErr + "）");
    }
    if (placed != NoReplaceStatus::Ok) {
        return Fail(InstallError::PublishFailed, noReplaceErr);
    }

    // 5) 发布已经发生（不可回滚）。刷盘对象是**已钉住的父目录 fd**，
    //    不是重新按路径解析得到的目录。失败只影响"断电后是否还在"，
    //    绝不能因此报告"未安装"。
    out.ok = true;
    out.published = true;

    // 发布后快照：确认"钉住父目录下的条目"与"按路径解析的 finalDir"仍是同一 inode。
    // 只做报告，不回滚；同 UID 可在本检查之后继续搬动，因此这不是可用性保证。
    out.pathStateUncertain =
        CheckPublishedPath(opened.fd, leaf, finalDir) != PublishedPathState::Ok;

    const bool parentSynced = ops.fsyncFd(opened.fd);
    if (!parentSynced) {
        out.durabilityUnknown = true;
    }

    if (out.pathStateUncertain) {
        out.detail =
            "已发布；但目标路径状态不确定（父目录可能已被移动/替换）。"
            "发布事实成立、不会回滚，但当前不保证该路径上的模型可用；请检查: " +
            finalDir;
    } else if (!parentSynced) {
        out.detail =
            "已发布；父目录刷盘失败，持久性未知（断电后可能丢失；发布事实成立，不回滚）";
    } else {
        out.detail = "已发布";
    }
    return out;
}

} // namespace fcitx::model_manager
