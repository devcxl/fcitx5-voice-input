#pragma once

// 把归档"取得"到调用方的私有暂存路径，供后续校验/解包使用。
//
// 为什么需要这一层：**绝不能**对用户给出的路径先算摘要、再按路径重开解包——
// 外部进程可在这两次 open 之间替换文件（TOCTOU），造成"校验的是 A、解包的是 B"。
// 因此离线归档也先以独占方式复制进私有暂存（有界、可取消），之后只操作副本。
// 下载路径同理：libcurl 直接写入私有暂存文件。

#include <cstdint>
#include <functional>
#include <string>

#include "model_catalog.h"

namespace fcitx::model_manager {

enum class ArchiveSourceStatus {
    Ok = 0,
    Cancelled,
    DownloadFailed,   // 网络/HTTP/写盘失败，或源路径不可读
    ArchiveMismatch,  // 体积超过清单声明（不读取内容）
    Internal,
};

const char* ArchiveSourceStatusName(ArchiveSourceStatus status);

struct ArchiveSourceResult {
    ArchiveSourceStatus status = ArchiveSourceStatus::Ok;
    std::string detail;
};

struct ArchiveSourceCallbacks {
    /// 下载进度（字节）。复制本地文件时不调用。
    std::function<void(uint64_t done, uint64_t total)> onProgress;
    /// 返回 true 表示要求中止。
    std::function<bool()> cancelled;
};

/// 把归档准备到 destPath（必须是本次暂存目录内的路径，调用方保证独占）。
///
/// configuredPath 为空 → 按清单固定 URL 下载；非空 → 复制该本地文件。
/// 复制/下载都以 spec.archiveSize 为硬上限：更大的输入在复制前（fstat）或
/// 复制中即被拒绝，不会被完整搬运。
ArchiveSourceResult ObtainPrivateArchive(const ModelSpec& spec,
                                         const std::string& configuredPath,
                                         const std::string& destPath,
                                         const ArchiveSourceCallbacks& callbacks);

} // namespace fcitx::model_manager
