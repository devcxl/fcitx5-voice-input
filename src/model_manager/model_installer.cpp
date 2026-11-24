#include "model_installer.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "archive_download.h"
#include "archive_extract.h"
#include "archive_source.h"
#include "fs_safe.h"
#include "safe_remove.h"
#include "install_lock.h"
#include "installed_model.h"
#include "model_paths.h"
#include "publish.h"
#include "sha256.h"
#include "trial_load.h"

namespace fcitx::model_manager {

namespace {

using Clock = std::chrono::steady_clock;

uint64_t ElapsedMs(Clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                             start)
            .count());
}

void Emit(const InstallOptions& options, InstallEvent event) {
    if (options.emit) options.emit(event);
}

bool IsCancelled(const InstallOptions& options) {
    return options.cancelled && options.cancelled();
}

/// 确保目录存在。只在**本次创建**时设置权限，绝不对已存在的用户目录 chmod。
///
/// 信任边界：`models` 的父目录（<data>/fcitx5/voice-input）及更上层允许是
/// 符号链接（用户 HOME/XDG 常见），从 `models` 起逐级要求真实目录。
bool MakeDirs(const std::string& path, unsigned mode) {
    std::string err;
    return EnsureDirUnder(path, ModelsParentDir(), mode, &err);
}

/// 逐文件校验结果的安装器内部分类。
struct FileCheck {
    bool ok = false;
    InstallError error = InstallError::None;
    std::string detail;
};

/// 把共享的 InstalledCheck 映射为安装器内部的失败分类。
FileCheck VerifyInstalledFiles(const ModelSpec& spec, const std::string& dir,
                               const std::function<bool()>& cancelled) {
    const InstalledCheck check = CheckInstalledModel(spec, dir, cancelled);
    if (check.cancelled) {
        FileCheck out;
        out.ok = false;
        out.error = InstallError::Cancelled;
        out.detail = "校验已取消";
        return out;
    }
    FileCheck out;
    if (check.state == InstalledState::Valid) {
        out.ok = true;
        return out;
    }
    out.ok = false;
    out.detail = check.detail;
    switch (check.state) {
        case InstalledState::Absent:
            out.error = InstallError::MissingFile;
            break;
        case InstalledState::Tampered:
            out.error = InstallError::FileMismatch;
            break;
        default:
            out.error = InstallError::Internal;
            break;
    }
    return out;
}

} // namespace

InstallOutcome InstallModel(const ModelSpec& spec,
                            const InstallOptions& options) {
    const Clock::time_point start = Clock::now();
    InstallOutcome outcome;

    auto finish = [&](InstallError error, bool published) {
        outcome.error = error;
        outcome.published = published;
        outcome.cancelled = IsCancelled(options);
        outcome.ok = error == InstallError::None;
        outcome.elapsedMs = ElapsedMs(start);
        InstallEvent event;
        event.stage = InstallStage::Result;
        event.modelId = spec.id;
        event.version = spec.version;
        event.ok = outcome.ok;
        event.published = outcome.published;
        event.cancelled = outcome.cancelled;
        event.durabilityUnknown = outcome.durabilityUnknown;
        event.pathStateUncertain = outcome.pathStateUncertain;
        event.error = outcome.error;
        event.detail = outcome.detail;
        event.versionDir = outcome.versionDir;
        event.elapsedMs = outcome.elapsedMs;
        Emit(options, event);
        return outcome;
    };

    auto fail = [&](InstallError error, std::string detail) {
        outcome.detail = std::move(detail);
        return finish(error, /*published=*/false);
    };

    if (!IsSafePathSegment(spec.id) || !IsSafePathSegment(spec.version)) {
        return fail(InstallError::UnsafePath, "清单中的 id/version 不安全");
    }

    const std::string root = DataRootDir();
    if (root.empty()) {
        return fail(InstallError::Internal,
                    "无法确定用户数据目录（XDG_DATA_HOME/HOME 均不可用）");
    }

    const std::string finalDir = VersionDir(spec.id, spec.version);
    if (finalDir.empty()) {
        return fail(InstallError::UnsafePath, "版本目录路径非法");
    }

    InstallEvent startEvent;
    startEvent.stage = InstallStage::Start;
    startEvent.modelId = spec.id;
    startEvent.version = spec.version;
    Emit(options, startEvent);

    if (!MakeDirs(root, 0755)) {
        return fail(InstallError::Internal, "无法创建模型根目录");
    }

    // 锁必须先于任何读写，保证同模型并发安装串行化。
    const std::string locksDir = LocksDir();
    if (!MakeDirs(locksDir, 0700)) {
        return fail(InstallError::Internal, "无法创建锁目录");
    }
    InstallLock lock;
    std::string lockDetail;
    const LockStatus lockStatus =
        lock.TryAcquire(locksDir + "/" + spec.id + ".lock", &lockDetail);
    if (lockStatus == LockStatus::Busy) {
        return fail(InstallError::LockBusy,
                    "另一个安装进程正在安装该模型，请稍后重试");
    }
    if (lockStatus != LockStatus::Acquired) {
        return fail(InstallError::Internal, lockDetail);
    }

    // 幂等：目标已存在且逐文件校验通过 → 直接成功，不重复下载。
    // 只接受**真实目录**（拒绝符号链接充当版本目录，避免指向别处）。
    std::error_code statusEc;
    const auto finalStatus = std::filesystem::symlink_status(finalDir, statusEc);
    // 非 ENOENT 的错误（权限、I/O、路径异常）必须立即失败：
    // 绝不能把它当成"目标不存在"，否则发布阶段会去覆盖一个我们其实看不懂的路径。
    if (statusEc && statusEc != std::errc::no_such_file_or_directory) {
        return fail(InstallError::PublishFailed,
                    "无法确认版本目录状态: " + statusEc.message() + "（" +
                        finalDir + "）");
    }
    const bool finalIsRealDir =
        !statusEc && std::filesystem::is_directory(finalStatus) &&
        !std::filesystem::is_symlink(finalStatus);
    if (finalIsRealDir) {
        const FileCheck existing =
            VerifyInstalledFiles(spec, finalDir, [&] { return IsCancelled(options); });
        if (existing.error == InstallError::Cancelled) {
            return fail(InstallError::Cancelled, "安装已取消");
        }
        if (existing.ok) {
            outcome.versionDir = finalDir;
            return finish(InstallError::None, /*published=*/false);
        }
        // 已存在但不完整/被篡改：不在原地修补，走暂存 + 原子替换路径；
        // 旧目录保留到发布成功之前（发布时会先移开，见下）。
    }

    if (IsCancelled(options)) {
        return fail(InstallError::Cancelled, "安装已取消");
    }

    // ── 暂存区（与最终目录同文件系统）─────────────────────────────────────
    const std::string stagingRoot = StagingRoot();
    if (!MakeDirs(stagingRoot, 0700)) {
        return fail(InstallError::Internal, "无法创建暂存目录");
    }
    // mkdtemp 已以独占方式创建 0700 目录；不需要也不应再 chmod。
    std::string stagingErr;
    const std::string stagingDir =
        MakeUniqueDir(stagingRoot, spec.id, &stagingErr);
    if (stagingDir.empty()) {
        return fail(InstallError::Internal,
                    "无法创建本次暂存目录: " + stagingErr);
    }
    // 从创建那一刻起钉住 fd：只要它还开着，这个 inode 就不会被回收复用，
    // 后续"目标是否仍是同一目录"的比较才可靠（单凭历史 dev/ino 值不可靠）。
    std::string identityErr;
    const PinnedDir stagingPin = PinnedDir::Open(stagingDir, &identityErr);
    if (!stagingPin.valid()) {
        return fail(InstallError::Internal,
                    "无法钉住暂存目录: " + identityErr);
    }
    // 受控相对路径：相对 ModelsParentDir()（<data>/fcitx5/voice-input）。
    const std::string stagingRelative =
        "models/.staging/" +
        std::filesystem::path(stagingDir).filename().string();

    // 离开作用域时清理本次暂存：成功发布后内容已移入版本目录，
    // 失败/取消时半成品不得留在最终路径下。其它**版本**目录从不删除。
    // 清理守卫必须在两个钉住对象**之后**声明：析构按声明逆序，守卫先跑，
    // 此时 stagingPin 仍打开，inode 不可能被复用，身份比较可靠。
    struct StagingGuard {
        std::string relative;
        std::string trustedBase;
        int pinnedFd;
        ~StagingGuard() {
            if (relative.empty()) return;
            std::string err;
            if (!RemoveTreePinned(trustedBase, relative, pinnedFd, &err)) {
                std::fprintf(stderr,
                             "model_installer: 暂存目录未删除（保守保留）: %s\n",
                             err.c_str());
            }
        }
    } guard{stagingRelative, ModelsParentDir(), stagingPin.fd()};

    // ── 1. 取得归档（一律先落到私有暂存，再校验/解包）─────────────────────
    const std::string privateArchive = stagingDir + "/archive.tar.bz2";
    if (options.archivePath.empty()) {
        InstallEvent dl;
        dl.stage = InstallStage::Download;
        dl.modelId = spec.id;
        dl.bytesTotal = spec.archiveSize;
        Emit(options, dl);
    }
    ArchiveSourceCallbacks sourceCallbacks;
    sourceCallbacks.cancelled = [&] { return IsCancelled(options); };
    sourceCallbacks.onProgress = [&](uint64_t done, uint64_t total) {
        // 节流由下载层完成（archive_source），这里只负责转成协议事件。
        InstallEvent p;
        p.stage = InstallStage::Download;
        p.modelId = spec.id;
        p.bytesDone = done;
        p.bytesTotal = total;
        Emit(options, p);
    };
    const ArchiveSourceResult source =
        ObtainPrivateArchive(spec, options.archivePath, privateArchive,
                             sourceCallbacks);
    if (source.status != ArchiveSourceStatus::Ok) {
        switch (source.status) {
            case ArchiveSourceStatus::Cancelled:
                return fail(InstallError::Cancelled, "取得归档已取消");
            case ArchiveSourceStatus::ArchiveMismatch:
                return fail(InstallError::ArchiveMismatch, source.detail);
            case ArchiveSourceStatus::Internal:
                return fail(InstallError::Internal, source.detail);
            default:
                return fail(InstallError::DownloadFailed, source.detail);
        }
    }

    // ── 2. 校验归档摘要（有界 + 可取消）─────────────────────────────────
    {
        InstallEvent e;
        e.stage = InstallStage::VerifyArchive;
        e.modelId = spec.id;
        e.version = spec.version;
        Emit(options, e);
    }

    // 上限取清单声明大小：更大的文件在 fstat 阶段即被拒绝，不会被完整读取。
    const FileDigestResult archiveDigest =
        DigestFile(privateArchive, spec.archiveSize,
                   [&] { return IsCancelled(options); });
    if (archiveDigest.status == FileDigestStatus::Cancelled) {
        return fail(InstallError::Cancelled, "校验归档已取消");
    }
    if (archiveDigest.status == FileDigestStatus::TooLarge) {
        return fail(InstallError::ArchiveMismatch,
                    "归档大于清单声明的大小");
    }
    if (archiveDigest.status != FileDigestStatus::Ok) {
        return fail(InstallError::DownloadFailed,
                    std::string("无法读取归档以校验摘要: ") +
                        FileDigestStatusName(archiveDigest.status));
    }
    if (ToHex(archiveDigest.digest) != spec.archiveSha256Hex ||
        archiveDigest.size != spec.archiveSize) {
        return fail(InstallError::ArchiveMismatch,
                    "归档摘要或大小与清单不一致");
    }

    if (IsCancelled(options)) {
        return fail(InstallError::Cancelled, "安装已取消");
    }

    // ── 3. 白名单解包 ───────────────────────────────────────────────────
    const std::string contentDir = stagingDir + "/content";
    if (!MakeDirs(contentDir, 0700)) {
        return fail(InstallError::Internal, "无法创建解包目录");
    }
    const PinnedDir contentPin = PinnedDir::Open(contentDir, &identityErr);
    if (!contentPin.valid()) {
        return fail(InstallError::Internal,
                    "无法钉住解包目录: " + identityErr);
    }

    ExtractCallbacks ec;
    ec.onFileStart = [&](const std::string& relative) {
        InstallEvent e;
        e.stage = InstallStage::Extract;
        e.modelId = spec.id;
        e.file = relative;
        Emit(options, e);
    };
    ec.cancelled = [&] { return IsCancelled(options); };

    const ExtractResult extract =
        ExtractWhitelisted(privateArchive, spec, contentDir, ec);
    if (extract.status != ExtractStatus::Ok) {
        switch (extract.status) {
            case ExtractStatus::Cancelled:
                return fail(InstallError::Cancelled, "解包已取消");
            case ExtractStatus::UnsafeEntry:
                return fail(InstallError::ArchiveUnsafe, extract.detail);
            case ExtractStatus::LimitExceeded:
                return fail(InstallError::ArchiveUnsafe, extract.detail);
            case ExtractStatus::MissingFile:
                return fail(InstallError::MissingFile, extract.detail);
            case ExtractStatus::OpenFailed:
                return fail(InstallError::ExtractFailed, extract.detail);
            default:
                return fail(InstallError::ExtractFailed, extract.detail);
        }
    }

    // ── 4. 逐文件校验（解包时已算摘要，这里再与清单比对）────────────────
    for (size_t i = 0; i < spec.fileCount; ++i) {
        const ModelFileSpec& want = spec.files[i];
        const auto it = extract.files.find(want.relativePath);
        if (it == extract.files.end()) {
            return fail(InstallError::MissingFile,
                        std::string("缺少文件: ") + want.relativePath);
        }
        InstallEvent e;
        e.stage = InstallStage::VerifyFile;
        e.modelId = spec.id;
        e.file = want.relativePath;
        Emit(options, e);

        if (it->second.size != want.size ||
            it->second.sha256Hex != want.sha256Hex) {
            return fail(InstallError::FileMismatch,
                        std::string("文件校验失败: ") + want.relativePath);
        }
    }

    // 独立再核一次落盘内容：不信任上面的内存记录，避免"算过的"和"写下去的"不一致。
    const FileCheck staged =
        VerifyInstalledFiles(spec, contentDir, [&] { return IsCancelled(options); });
    if (!staged.ok) {
        return fail(staged.error, staged.detail);
    }

    if (!WriteInstallMarker(spec, contentDir)) {
        return fail(InstallError::Internal, "无法写入 install.json");
    }

    if (IsCancelled(options)) {
        return fail(InstallError::Cancelled, "安装已取消");
    }

    // ── 5. 子进程试加载 ─────────────────────────────────────────────────
    if (options.runTrialLoad) {
        InstallEvent e;
        e.stage = InstallStage::VerifyTrial;
        e.modelId = spec.id;
        Emit(options, e);

        TrialOptions trial;
        trial.timeoutMs = options.trialTimeoutMs;
        if (options.cancelled) {
            trial.cancelled = options.cancelled;
        }

        const TrialResult trialResult =
            RunTrialLoad(spec, contentDir, trial);
        switch (trialResult.status) {
            case TrialStatus::Ok:
                break;
            case TrialStatus::Cancelled:
                return fail(InstallError::Cancelled, "试加载已取消");
            case TrialStatus::Timeout:
                return fail(InstallError::TrialLoadTimeout,
                            "试加载超过时限，已终止子进程");
            case TrialStatus::Unavailable:
                return fail(InstallError::TrialLoadFailed,
                            "找不到可用的 sherpa 运行库，无法完成试加载");
            case TrialStatus::SpawnFailed:
                return fail(InstallError::Internal,
                            "无法启动试加载子进程: " + trialResult.stderrTail);
            case TrialStatus::Failed:
            default: {
                std::string detail = "试加载失败";
                if (trialResult.termSignal != 0) {
                    detail += "（子进程被信号 " +
                              std::to_string(trialResult.termSignal) +
                              " 终止）";
                } else if (trialResult.exitCode >= 0) {
                    detail += "（退出码 " +
                              std::to_string(trialResult.exitCode) + "）";
                }
                if (!trialResult.stderrTail.empty()) {
                    detail += ": " + trialResult.stderrTail;
                }
                return fail(InstallError::TrialLoadFailed, detail);
            }
        }
    }

    if (IsCancelled(options)) {
        return fail(InstallError::Cancelled, "安装已取消");
    }

    // ── 6. 原子发布 ─────────────────────────────────────────────────────
    //
    // 这是取消的临界点：越过 rename 之后版本目录已生效，后续无论发生什么都不能
    // 报告"未安装"。因此这里先做取消检查，rename 成功后立即置 published。
    {
        InstallEvent e;
        e.stage = InstallStage::Publish;
        e.modelId = spec.id;
        e.version = spec.version;
        Emit(options, e);
    }

    const std::string modelDir = ModelDir(spec.id);
    if (!MakeDirs(modelDir, 0755)) {
        return fail(InstallError::PublishFailed, "无法创建模型目录");
    }

    bool published = false;
    const bool finalExistedUpFront =
        !statusEc && finalStatus.type() != std::filesystem::file_type::not_found;

    // 廉价早退：避免已取消时白做刷盘/打开目录。
    // **真正的线性化点在 PublishVersionDir 内部**：内容 fsync 之后、拿到目标
    // 父目录 fd 之后、紧邻 renameat2 之前还有一次最终检查（覆盖 openat 阻塞
    // 期间的取消）。一旦该 rename 成功，版本目录即已生效，之后一律按成功报告。
    if (IsCancelled(options)) {
        return fail(InstallError::Cancelled, "安装已取消（发布前）");
    }

    PublishSource publishSource;
    publishSource.dir = contentDir;
    publishSource.relativeToModelsParent = stagingRelative + "/content";
    publishSource.pinnedFd = contentPin.fd();

    const PublishOutcome publishResult = PublishVersionDir(
        spec, publishSource, finalDir, finalExistedUpFront,
        [&] { return IsCancelled(options); });
    if (!publishResult.ok) {
        return fail(publishResult.error, publishResult.detail);
    }
    published = publishResult.published;
    // 发布已生效：即使随后收到取消，也按成功报告（不得误报未安装）。
    outcome.versionDir = finalDir;
    outcome.durabilityUnknown = publishResult.durabilityUnknown;
    outcome.pathStateUncertain = publishResult.pathStateUncertain;
    if (!publishResult.detail.empty()) outcome.detail = publishResult.detail;
    return finish(InstallError::None, published);
}

} // namespace fcitx::model_manager
