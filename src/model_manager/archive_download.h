#pragma once

// 归档下载器：只访问清单里的固定 URL，不跟随重定向到非 HTTPS 目标。
//
// 安全要点：
//   - URL 来自编译期清单，不接受用户输入；
//   - 只允许 https（重定向目标同样校验）；
//   - 体积上限取清单的期望字节数，超出即中止（不信 Content-Length）；
//   - 写入调用方指定的暂存路径，原子性由调用方（同文件系统 rename）保证；
//   - 可取消：进度回调返回 false 即中止。

#include <cstdint>
#include <functional>
#include <string>

#include "model_catalog.h"

namespace fcitx::model_manager {

enum class DownloadStatus {
    Ok = 0,
    Cancelled,
    NetworkError,
    HttpError,
    TooLarge,
    IoError,
};

const char* DownloadStatusName(DownloadStatus status);

struct DownloadCallbacks {
    /// 进度：已下载字节 / 期望总字节。返回 false 表示中止。
    std::function<bool(uint64_t done, uint64_t total)> onProgress;
    /// 外部取消检查（可与 onProgress 结合）。
    std::function<bool()> cancelled;
};

struct DownloadResult {
    DownloadStatus status = DownloadStatus::Ok;
    uint64_t bytes = 0;
    std::string detail;
};

/// 把 spec.archiveUrl 下载到 destPath（覆盖写）。expectedSize 来自清单，
/// 用于硬上限，防止恶意服务端无限推送。
DownloadResult DownloadArchive(const ModelSpec& spec, const std::string& destPath,
                               const DownloadCallbacks& callbacks);

} // namespace fcitx::model_manager
