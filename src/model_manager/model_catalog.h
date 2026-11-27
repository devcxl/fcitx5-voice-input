#pragma once

// 受控模型清单：固定模型 ID / 版本 / 文件角色 / 逐文件 SHA-256 与尺寸。
//
// 设计约束（对应 docs/development/research/sherpa-model-install-and-loading.md
// 的 M1/M2/M3/M5）：
//
//   1. 清单是**唯一**的身份来源。模型目录里的任何自报信息（install.json、
//      目录名、文件大小）都不可信，只能用于快速否定（"缺文件"= 肯定没装好），
//      不能用于肯定（"文件对得上"必须逐个比对下面的编译期常量）。
//   2. 只有归档摘要是不够的：归档在安装时被解包后即不再被读取，发布后的文件
//      可能被篡改、截断或替换。因此每个会被加载的文件都有独立的 sha256 + size。
//   3. 本头文件是**纯数据 + const 查询**，不依赖网络、文件系统或安装器逻辑，
//      addon 运行时可以直接 include 它做校验，无需引入安装/下载依赖。
//
// 本文件不含任何写盘、下载或解包逻辑，也不应包含它们。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fcitx::model_manager {

/// 一个受控模型在版本目录内的单个文件。
///
/// relativePath 是解包后版本目录中的**精确文件名**，不含目录分隔符；
/// 调用方按 relativePath 拼接路径，不做前缀探测或模糊匹配。
struct ModelFileSpec {
    /// 文件角色： "encoder" / "decoder" / "joiner" / "tokens"。
    /// 角色决定它被喂给 sherpa 的哪个配置字段，与文件名无关。
    const char* role;
    /// 版本目录内的精确文件名。
    const char* relativePath;
    /// 期望内容的 SHA-256，64 位小写十六进制。
    const char* sha256Hex;
    /// 期望内容的精确字节数。
    uint64_t size;
};

/// 一个受控模型的完整描述。
///
/// id 是稳定的、由我们定义的模型种类标识（不是上游目录名），用于配置、
/// 日志与命令行参数；version 是不可变的上游版本目录名，构成落盘路径的一段。
/// 两者都不可由用户输入替代。
struct ModelSpec {
    /// 稳定模型 ID： "online-zipformer" / "offline-x-asr"。
    const char* id;
    /// 不可变上游版本目录名，同时是落盘路径的一段。
    const char* version;
    /// 面向配置/日志的后端标识，例如 "sherpa-onnx-streaming"。
    const char* backendId;
    /// true 表示流式（online）模型，false 表示非流式（offline）模型。
    bool streaming;
    /// 官方归档的下载地址（固定 URL，不含用户可控部分）。
    const char* archiveUrl;
    /// 官方归档的 SHA-256，64 位小写十六进制。
    const char* archiveSha256Hex;
    /// 官方归档的精确字节数。
    uint64_t archiveSize;
    /// 归档内需要落盘的文件，顺序即校验顺序。
    const ModelFileSpec* files;
    /// files 的元素个数。
    size_t fileCount;
};

/// 全部受控模型的静态数组，长度写入 *count（count 可为 nullptr）。
/// 返回的指针指向静态存储，生命周期与进程相同，调用方不得释放。
const ModelSpec* ListModelSpecs(size_t* count);

/// 按稳定模型 ID 查找；未知 ID 或空 ID 返回 nullptr。
const ModelSpec* FindModelSpec(std::string_view id);

/// 按文件角色查找（"encoder" 等）；未命中返回 nullptr。
const ModelFileSpec* FindModelFileByRole(const ModelSpec& spec,
                                         std::string_view role);

/// 按版本目录内文件名精确查找；未命中返回 nullptr。
const ModelFileSpec* FindModelFile(const ModelSpec& spec,
                                   std::string_view relativePath);

/// 该模型需要落盘的全部文件（即 files/fileCount），供安装器遍历。
/// 返回值来自清单本身，不是用户输入。
inline bool IsKnownRole(std::string_view role) {
    return role == "encoder" || role == "decoder" || role == "joiner" ||
           role == "tokens";
}

} // namespace fcitx::model_manager
