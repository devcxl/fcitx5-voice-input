#pragma once

// 白名单解包器：只把清单声明的文件写到目标目录，其余一律不落盘。
//
// 威胁模型：归档来自网络，必须视为不可信输入。
//   - 条目路径含 ".."、绝对路径、空组件 → 拒绝整个归档；
//   - 符号链接、硬链接、设备/FIFO/socket 等特殊文件 → 拒绝整个归档（不跟随、不创建）；
//   - 重复的清单文件 → 拒绝（避免"后写覆盖先写"的歧义）；
//   - 体积上限直接取清单中的**期望字节数**，超出即中止（不依赖归档自报大小）；
//   - 归档中未登记但无害的条目（README.md、test_wavs/ 等）**跳过、不解包**。
//
// 解包同时计算每个文件 SHA-256，避免对大文件读两遍；校验结果由调用方比对清单。

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "model_catalog.h"

namespace fcitx::model_manager {

/// 解包结果分类。
enum class ExtractStatus {
    Ok = 0,
    OpenFailed,      // 归档无法打开/不是支持的格式
    UnsafeEntry,     // 越界/链接/特殊文件/重复条目
    LimitExceeded,   // 条目数或体积超过清单约束
    MissingFile,     // 归档内缺少清单要求的文件
    IoError,         // 写盘失败
    Cancelled,       // 调用方取消
};

const char* ExtractStatusName(ExtractStatus status);

struct ExtractedFile {
    std::string relativePath;
    uint64_t size = 0;
    std::string sha256Hex;
    bool digestComputed = false;
};

struct ExtractResult {
    ExtractStatus status = ExtractStatus::Ok;
    /// 失败条目的诊断（不含敏感信息）。
    std::string detail;
    /// 已成功解包的文件，按 relativePath 索引。
    std::map<std::string, ExtractedFile> files;
};

struct ExtractCallbacks {
    /// 每开始处理一个清单文件时调用（relativePath）。
    std::function<void(const std::string& relativePath)> onFileStart;
    /// 每完成一个清单文件时调用。
    std::function<void(const ExtractedFile& file)> onFileDone;
    /// 返回 true 表示要求中止。
    std::function<bool()> cancelled;
};

/// 把 archivePath 里清单登记的条目解包到 destDir（必须已存在、为空、由调用方
/// 控制权限）。spec 决定白名单、精确文件名与体积上限。
ExtractResult ExtractWhitelisted(const std::string& archivePath,
                                 const ModelSpec& spec,
                                 const std::string& destDir,
                                 const ExtractCallbacks& callbacks);

} // namespace fcitx::model_manager
