#include "archive_source.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "archive_download.h"
#include "fs_safe.h"

namespace fcitx::model_manager {

/// 把归档准备到 destPath（本次暂存的私有副本）。
///
/// 离线安装时**必须**复制而不是直接引用用户路径：否则"先算摘要、再按路径
/// 重开解包"之间存在 TOCTOU，外部可把文件换掉。复制有界（清单声明大小）
/// 且可取消。
ArchiveSourceResult ObtainPrivateArchive(const ModelSpec& spec,
                                         const std::string& configuredPath,
                                         const std::string& destPath,
                                    const ArchiveSourceCallbacks& callbacks) {
    //
    // 为什么离线归档也要复制：若直接对用户给的路径先 HashFile 再按路径重开解包，
    // 外部进程可在这两次 open 之间把文件换掉（TOCTOU），导致"校验的是 A、解包的是 B"。
    // 因此先以独占方式复制进本次暂存的私有目录（有界、可取消），后续所有操作
    // 都针对这份私有副本。
    const bool ownArchive = configuredPath.empty();

    if (ownArchive) {
        // 节流放在下载层：每 1 MiB 或每 1% 回调一次，终值只报一次。
        // 这样调用方（GUI 适配层）不必自己实现节流，也不会被刷屏。
        DownloadCallbacks cb;
        uint64_t lastReported = 0;
        bool reportedFinal = false;
        cb.onProgress = [&](uint64_t done, uint64_t total) {
            const uint64_t total2 = total > 0 ? total : spec.archiveSize;
            const bool isFinal = total2 > 0 && done >= total2;
            if (isFinal && reportedFinal) return true;
            const bool crossed = done - lastReported >= 1024 * 1024;
            const bool percent = total2 > 0 &&
                                 (done * 100 / total2) !=
                                     (lastReported * 100 / total2);
            if (crossed || percent || isFinal) {
                lastReported = done;
                if (isFinal) reportedFinal = true;
                if (callbacks.onProgress) callbacks.onProgress(done, total2);
            }
            return !(callbacks.cancelled && callbacks.cancelled());
        };
        cb.cancelled = [&] {
            return callbacks.cancelled && callbacks.cancelled();
        };

        const DownloadResult dlResult =
            DownloadArchive(spec, destPath, cb);
        if (dlResult.status != DownloadStatus::Ok) {
            if (dlResult.status == DownloadStatus::Cancelled) {
                return {ArchiveSourceStatus::Cancelled, "下载已取消"};
            }
            if (dlResult.status == DownloadStatus::TooLarge) {
                return {ArchiveSourceStatus::ArchiveMismatch, dlResult.detail};
            }
            return {ArchiveSourceStatus::DownloadFailed, dlResult.detail};
        }
    } else {
        // 离线安装：复制到私有暂存。复制过程有界（清单声明大小）且可取消，
        // 源文件从不被直接读取为"待解包的归档"。
        std::string copyErr;
        const CopyStatus copyStatus = CopyFileBounded(
            configuredPath, destPath, spec.archiveSize,
            [&] { return callbacks.cancelled && callbacks.cancelled(); },
            &copyErr);
        switch (copyStatus) {
            case CopyStatus::Ok:
                break;
            case CopyStatus::Cancelled:
                return {ArchiveSourceStatus::Cancelled, "复制归档已取消"};
            case CopyStatus::SourceTooLarge:
                return {ArchiveSourceStatus::ArchiveMismatch,
                        "归档大于清单声明的大小: " + copyErr};
            case CopyStatus::SourceNotRegular:
            case CopyStatus::IoError:
            default:
                return {ArchiveSourceStatus::DownloadFailed,
                        "无法读取指定的归档: " + copyErr};
        }
    }

    return {ArchiveSourceStatus::Ok, {}};
}


const char* ArchiveSourceStatusName(ArchiveSourceStatus status) {
    switch (status) {
        case ArchiveSourceStatus::Ok: return "ok";
        case ArchiveSourceStatus::Cancelled: return "cancelled";
        case ArchiveSourceStatus::DownloadFailed: return "download_failed";
        case ArchiveSourceStatus::ArchiveMismatch: return "archive_mismatch";
        case ArchiveSourceStatus::Internal: return "internal";
    }
    return "internal";
}

} // namespace fcitx::model_manager
