#include "sha256.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>

namespace fcitx::model_manager {

namespace {

constexpr size_t kChunk = 64 * 1024;

bool HashFd(int fd, uint64_t maxBytes, Sha256Digest* out, uint64_t* size,
            const std::function<bool()>* cancelled = nullptr) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1;

    uint64_t total = 0;
    // 复用一个固定缓冲区，避免按文件大小分配。
    static thread_local unsigned char buffer[kChunk];

    while (ok) {
        if (cancelled && *cancelled && (*cancelled)()) return false;
        const ssize_t n = ::read(fd, buffer, kChunk);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        total += static_cast<uint64_t>(n);
        if (maxBytes > 0 && total > maxBytes) {
            ok = false;
            break;
        }
        ok = EVP_DigestUpdate(ctx, buffer, static_cast<size_t>(n)) == 1;
    }

    unsigned int len = 0;
    if (ok) {
        ok = EVP_DigestFinal_ex(ctx, out->data(), &len) == 1 && len == 32;
    }
    EVP_MD_CTX_free(ctx);
    if (ok && size) *size = total;
    return ok;
}

} // namespace

std::string ToHex(const Sha256Digest& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint8_t byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

bool ParseHexDigest(std::string_view hex, Sha256Digest* out) {
    if (!out || hex.size() != 64) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < 32; ++i) {
        const int hi = nibble(hex[i * 2]);
        const int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        (*out)[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

Sha256::Sha256() {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return;
    }
    ctx_ = ctx;
}

Sha256::~Sha256() {
    if (ctx_) EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx_));
}

void Sha256::Update(const void* data, size_t len) {
    if (!ctx_ || !data || len == 0) return;
    if (EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(ctx_), data, len) != 1) {
        EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx_));
        ctx_ = nullptr;
    }
}

bool Sha256::Final(Sha256Digest* out) {
    if (!ctx_ || !out) return false;
    unsigned int len = 0;
    const bool ok = EVP_DigestFinal_ex(static_cast<EVP_MD_CTX*>(ctx_),
                                       out->data(), &len) == 1 &&
                    len == 32;
    EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx_));
    ctx_ = nullptr;
    return ok;
}

bool HashFile(const std::string& path, Sha256Digest* out, uint64_t* size) {
    return HashFileLimited(path, 0, out, size);
}

bool HashFileLimited(const std::string& path, uint64_t maxBytes,
                     Sha256Digest* out, uint64_t* size) {
    const FileDigestResult result = DigestFile(path, maxBytes);
    if (result.status != FileDigestStatus::Ok || !out) return false;
    *out = result.digest;
    if (size) *size = result.size;
    return true;
}

const char* FileDigestStatusName(FileDigestStatus status) {
    switch (status) {
        case FileDigestStatus::Ok: return "ok";
        case FileDigestStatus::OpenFailed: return "open_failed";
        case FileDigestStatus::NotRegularFile: return "not_regular_file";
        case FileDigestStatus::TooLarge: return "too_large";
        case FileDigestStatus::ReadFailed: return "read_failed";
        case FileDigestStatus::Cancelled: return "cancelled";
    }
    return "read_failed";
}

FileDigestResult DigestFile(const std::string& path, uint64_t limitBytes,
                            const std::function<bool()>& cancelled) {
    FileDigestResult result;

    if (path.empty()) {
        result.status = FileDigestStatus::OpenFailed;
        return result;
    }

    // O_NOFOLLOW：符号链接不是受控内容。
    // O_NONBLOCK：路径若是 FIFO，普通 open 会永久阻塞；对普通文件无影响。
    const int fd = ::open(path.c_str(),
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        result.status = FileDigestStatus::OpenFailed;
        return result;
    }

    // 类型与大小都在 fd 上判定，避免 lstat/open 之间的类型替换。
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        result.status = FileDigestStatus::OpenFailed;
        return result;
    }
    if (!S_ISREG(st.st_mode)) {
        ::close(fd);
        result.status = FileDigestStatus::NotRegularFile;
        return result;
    }

    result.size = static_cast<uint64_t>(st.st_size);
    // 先按 fstat 大小判断：超限直接失败，不读取任何内容。
    if (limitBytes > 0 && result.size > limitBytes) {
        ::close(fd);
        result.status = FileDigestStatus::TooLarge;
        return result;
    }

    uint64_t hashedSize = 0;
    bool ok = false;
    if (cancelled) {
        ok = HashFd(fd, limitBytes, &result.digest, &hashedSize, &cancelled);
    } else {
        ok = HashFd(fd, limitBytes, &result.digest, &hashedSize, nullptr);
    }
    ::close(fd);

    if (!ok) {
        // 读取中途超限或取消：区分两种失败，便于调用方给出准确诊断。
        if (cancelled && cancelled()) {
            result.status = FileDigestStatus::Cancelled;
        } else {
            result.status = FileDigestStatus::ReadFailed;
        }
        return result;
    }

    result.size = hashedSize;
    result.digestValid = true;
    result.status = FileDigestStatus::Ok;
    return result;
}

bool HashBuffer(const void* data, size_t len, Sha256Digest* out) {
    if (!out) return false;
    // 空指针 + 非零长度是无效调用：直接失败，绝不按 len 读取。
    if (!data && len > 0) return false;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1;
    if (ok && len > 0) {
        ok = EVP_DigestUpdate(ctx, data, len) == 1;
    }
    unsigned int outLen = 0;
    if (ok) {
        ok = EVP_DigestFinal_ex(ctx, out->data(), &outLen) == 1 && outLen == 32;
    }
    EVP_MD_CTX_free(ctx);
    return ok;
}

} // namespace fcitx::model_manager
