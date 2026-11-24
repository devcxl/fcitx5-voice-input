#pragma once

// 流式 SHA-256（基于 OpenSSL EVP，不自研密码学原语）。
//
// 依赖：libcrypto（OpenSSL 3.x 的 EVP 接口）。
// 注意：该依赖必须**显式**链接 `OpenSSL::Crypto` / `-lcrypto`。
// 不能指望通过 libcurl 传递获得——现代 ld 不再解析 DT_NEEDED 的间接符号，
// 且部分发行版的 libcurl 链 GnuTLS（根本没有 libcrypto）。
//
// 用途：安装器校验下载归档与解包文件、addon 运行时校验已落盘文件。
// 只读打开；失败通过返回值表达，不抛异常。

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace fcitx::model_manager {

using Sha256Digest = std::array<uint8_t, 32>;

/// 64 位小写十六进制。
std::string ToHex(const Sha256Digest& digest);

/// 解析 64 位十六进制（大小写均可）；长度或字符非法返回 false。
bool ParseHexDigest(std::string_view hex, Sha256Digest* out);

/// 增量摘要。
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    /// 是否可以参与摘要（构造失败时为 false）。
    bool valid() const { return ctx_ != nullptr; }

    void Update(const void* data, size_t len);

    /// 结束摘要。调用后对象不可复用；重复调用返回 false。
    bool Final(Sha256Digest* out);

private:
    void* ctx_ = nullptr;
};

/// 对文件全部内容做 SHA-256，同时返回字节数。
/// 任何 I/O 失败返回 false 且 out 内容无意义。
bool HashFile(const std::string& path, Sha256Digest* out, uint64_t* size);

/// 对内存缓冲区做 SHA-256。
bool HashBuffer(const void* data, size_t len, Sha256Digest* out);

/// 只读打开文件并计算摘要，用途同上但允许调用方先做尺寸预检。
bool HashFileLimited(const std::string& path, uint64_t maxBytes,
                     Sha256Digest* out, uint64_t* size);

/// 文件摘要的详细结果，用于精确区分"不可读"与"内容不符"。
enum class FileDigestStatus {
    Ok = 0,
    OpenFailed,       // 打不开（缺失、权限、符号链接 O_NOFOLLOW 拒绝）
    NotRegularFile,   // 是目录/FIFO/设备等，不是普通文件
    TooLarge,         // 声明上限非零且实际大小超过它（不会读取内容）
    ReadFailed,       // 读取中途失败
    Cancelled,        // 调用方取消
};

const char* FileDigestStatusName(FileDigestStatus status);

struct FileDigestResult {
    FileDigestStatus status = FileDigestStatus::OpenFailed;
    /// 实际字节数（OpenFailed/NotRegularFile 时为 0；TooLarge 时为 fstat 结果）。
    uint64_t size = 0;
    Sha256Digest digest{};
    bool digestValid = false;
};

/// 计算文件摘要，支持上限与取消。
///
/// 语义保证：
///   - `O_NOFOLLOW|O_NONBLOCK` 打开，`fstat` 确认是普通文件后才读取；
///   - `limitBytes > 0` 时**先**用 fstat 大小判断，超限立即返回 TooLarge，
///     不会读取内容（防止超大文件耗尽 I/O）；
///   - 读取过程中周期性检查 cancelled，可及时中止大文件；
///   - `limitBytes == 0` 表示无上限。
FileDigestResult DigestFile(const std::string& path, uint64_t limitBytes,
                            const std::function<bool()>& cancelled = {});

} // namespace fcitx::model_manager
