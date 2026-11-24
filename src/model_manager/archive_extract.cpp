#include "archive_extract.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <archive.h>
#include <archive_entry.h>

#include "sha256.h"

namespace fcitx::model_manager {

namespace {

/// 归档条目数上限：官方归档是十几个条目，留出充足余量但拒绝条目洪泛。
constexpr int kMaxEntries = 4096;

/// 路径是否安全（不允许绝对路径、空组件、".."、"." 组件、反斜杠与 NUL）。
bool IsSafeArchivePath(std::string_view path) {
    if (path.empty() || path.front() == '/') return false;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const size_t end = slash == std::string_view::npos ? path.size() : slash;
        const std::string_view component = path.substr(start, end - start);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        for (char c : component) {
            if (c == '\\' || c == '\0') return false;
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

/// 归档条目的分类结果。
enum class EntryKind {
    Skip,   // 安全但无关（目录、README、test_wavs/…），不解包
    Wanted, // 清单登记的文件，需要解包
    Unsafe, // 路径非法或越界，必须拒绝整个归档
};

struct EntryMapping {
    EntryKind kind = EntryKind::Skip;
    std::string relative;
};

/// 条目是否位于清单版本目录之内（含目录自身）。
bool IsInsideVersionDir(const ModelSpec& spec, std::string_view entryPath) {
    const std::string_view version = spec.version;
    if (entryPath == version) return true;
    if (entryPath.size() <= version.size()) return false;
    return entryPath.compare(0, version.size(), version) == 0 &&
           entryPath[version.size()] == '/';
}

/// 把归档内路径映射到清单相对路径：命中清单时为 "<version>/<whitelisted>"。
///
/// 判定原则（区分"不安全"与"无关"）：
///   - **不安全**：绝对路径、含 ".."/"."/空组件、含反斜杠或 NUL —— 这些是越界
///     或注入企图，必须拒绝整个归档。链接/特殊文件在调用方按 filetype 拒绝。
///   - **无关**：位于版本目录之外的第二套目录树，或版本目录内的 README.md、
///     test_onnx.py、test_wavs/、bpe.model 等清单外普通文件 —— 只是多余内容，
///     直接跳过不落盘即可。把它们当成错误会让官方归档无法安装。
EntryMapping MapArchiveEntry(const ModelSpec& spec, std::string_view entryPath,
                             bool isDirectory) {
    EntryMapping out;

    if (entryPath.empty() || entryPath.front() == '/') {
        out.kind = EntryKind::Unsafe;
        return out;
    }
    for (char c : entryPath) {
        if (c == '\\' || c == '\0') {
            out.kind = EntryKind::Unsafe;
            return out;
        }
    }

    // tar 的目录条目以 '/' 结尾；仅对目录允许并剥离一个尾部分隔符。
    const bool hadTrailingSlash = entryPath.size() > 1 && entryPath.back() == '/';
    if (hadTrailingSlash) {
        if (!isDirectory) {
            out.kind = EntryKind::Unsafe;
            return out;
        }
        entryPath.remove_suffix(1);
    }

    // 含 ".."/"."/空组件的路径是越界企图：拒绝，即使它落在版本目录之外。
    if (!IsSafeArchivePath(entryPath)) {
        out.kind = EntryKind::Unsafe;
        return out;
    }

    // 版本目录之外的条目：安全但无关，跳过。
    const std::string_view version = spec.version;
    if (entryPath == version) {
        out.kind = EntryKind::Skip;
        return out;
    }
    if (entryPath.size() <= version.size() ||
        entryPath.compare(0, version.size(), version) != 0 ||
        entryPath[version.size()] != '/') {
        out.kind = EntryKind::Skip;
        return out;
    }

    const std::string_view relative = entryPath.substr(version.size() + 1);
    if (relative.empty()) {
        out.kind = EntryKind::Skip;
        return out;
    }
    // 只接受清单登记的单层文件名；子路径（test_wavs/0.wav）安全但无关。
    if (relative.find('/') != std::string_view::npos) {
        out.kind = EntryKind::Skip;
        return out;
    }
    if (!FindModelFile(spec, relative)) {
        out.kind = EntryKind::Skip;
        return out;
    }
    if (isDirectory) {
        // 清单文件不应是目录。
        out.kind = EntryKind::Unsafe;
        return out;
    }

    out.kind = EntryKind::Wanted;
    out.relative = std::string(relative);
    return out;
}

bool WriteAll(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t written = 0;
    while (written < len) {
        const ssize_t n = ::write(fd, p + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

const char* ExtractStatusName(ExtractStatus status) {
    switch (status) {
        case ExtractStatus::Ok: return "ok";
        case ExtractStatus::OpenFailed: return "open_failed";
        case ExtractStatus::UnsafeEntry: return "unsafe_entry";
        case ExtractStatus::LimitExceeded: return "limit_exceeded";
        case ExtractStatus::MissingFile: return "missing_file";
        case ExtractStatus::IoError: return "io_error";
        case ExtractStatus::Cancelled: return "cancelled";
    }
    return "io_error";
}

ExtractResult ExtractWhitelisted(const std::string& archivePath,
                                 const ModelSpec& spec,
                                 const std::string& destDir,
                                 const ExtractCallbacks& callbacks) {
    ExtractResult result;

    if (destDir.empty()) {
        result.status = ExtractStatus::IoError;
        result.detail = "目标目录为空";
        return result;
    }

    archive* handle = archive_read_new();
    if (!handle) {
        result.status = ExtractStatus::OpenFailed;
        result.detail = "libarchive 初始化失败";
        return result;
    }

    // 只启用本安装器需要的格式/过滤器；不启用所有格式，避免接受意外容器。
    archive_read_support_filter_none(handle);
    archive_read_support_filter_bzip2(handle);
    archive_read_support_format_tar(handle);

    if (archive_read_open_filename(handle, archivePath.c_str(), 64 * 1024) !=
        ARCHIVE_OK) {
        result.status = ExtractStatus::OpenFailed;
        result.detail = archive_error_string(handle) ? archive_error_string(handle)
                                                     : "无法打开归档";
        archive_read_free(handle);
        return result;
    }

    // 复用缓冲区，避免按条目大小分配。
    std::vector<char> buffer(256 * 1024);
    int entryCount = 0;
    bool failed = false;

    while (!failed) {
        archive_entry* entry = nullptr;
        const int rc = archive_read_next_header(handle, &entry);
        if (rc == ARCHIVE_EOF) break;
        if (rc < ARCHIVE_OK) {
            result.status = ExtractStatus::OpenFailed;
            result.detail = archive_error_string(handle)
                                ? archive_error_string(handle)
                                : "归档头损坏";
            failed = true;
            break;
        }
        if (++entryCount > kMaxEntries) {
            result.status = ExtractStatus::LimitExceeded;
            result.detail = "归档条目数超过上限";
            failed = true;
            break;
        }

        const char* rawPath = archive_entry_pathname(entry);
        if (!rawPath) {
            result.status = ExtractStatus::UnsafeEntry;
            result.detail = "归档条目缺少路径";
            failed = true;
            break;
        }

        const auto fileType = archive_entry_filetype(entry);
        const bool isDirectory = fileType == AE_IFDIR;

        const EntryMapping mapping =
            MapArchiveEntry(spec, rawPath, isDirectory);
        if (mapping.kind == EntryKind::Unsafe) {
            result.status = ExtractStatus::UnsafeEntry;
            result.detail = std::string("归档条目路径不安全: ") + rawPath;
            failed = true;
            break;
        }

        // 链接与特殊文件：只有完全无关（不在版本目录内）的才允许跳过。
        // 一旦它们出现在版本目录内，就可能遮蔽白名单文件或把写入导向别处，
        // 因此拒绝整个归档。硬链接在 tar 里没有独立 filetype，用 hardlink 字段识别。
        const char* hardlink = archive_entry_hardlink(entry);
        const bool isHardlink = hardlink && *hardlink;
        const bool isSpecial = fileType == AE_IFLNK || fileType == AE_IFSOCK ||
                               fileType == AE_IFCHR || fileType == AE_IFBLK ||
                               fileType == AE_IFIFO || isHardlink;
        if (isSpecial) {
            if (IsInsideVersionDir(spec, rawPath)) {
                result.status = ExtractStatus::UnsafeEntry;
                result.detail = std::string("版本目录内包含链接或特殊文件: ") +
                                rawPath;
                failed = true;
                break;
            }
            // 归档里与模型无关的链接（例如上游打包脚本残留）：不落盘，跳过。
            archive_read_data_skip(handle);
            continue;
        }

        if (mapping.kind == EntryKind::Skip) {
            // 白名单外或目录条目：跳过（不落盘）。
            archive_read_data_skip(handle);
            continue;
        }
        const std::string& relative = mapping.relative;

        if (result.files.count(relative) > 0) {
            result.status = ExtractStatus::UnsafeEntry;
            result.detail = "归档中重复出现清单文件: " + relative;
            failed = true;
            break;
        }

        const ModelFileSpec* spec2 = FindModelFile(spec, relative);
        const uint64_t expectedSize = spec2 ? spec2->size : 0;

        if (callbacks.onFileStart) callbacks.onFileStart(relative);

        const std::string target = destDir + "/" + relative;
        const int fd = ::open(target.c_str(),
                              O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                              0644);
        if (fd < 0) {
            result.status = ExtractStatus::IoError;
            result.detail = "无法创建目标文件: " + relative;
            failed = true;
            break;
        }

        Sha256 hasher;
        uint64_t total = 0;
        bool writeOk = hasher.valid();

        while (writeOk) {
            if (callbacks.cancelled && callbacks.cancelled()) {
                result.status = ExtractStatus::Cancelled;
                failed = true;
                writeOk = false;
                break;
            }
            const la_ssize_t n = archive_read_data(handle, buffer.data(),
                                                   buffer.size());
            if (n == 0) break;
            if (n < 0) {
                result.status = ExtractStatus::OpenFailed;
                result.detail = archive_error_string(handle)
                                    ? archive_error_string(handle)
                                    : "读取归档数据失败";
                failed = true;
                writeOk = false;
                break;
            }
            total += static_cast<uint64_t>(n);
            // 体积上限取清单期望值：超出即为恶意/损坏，立即中止。
            if (expectedSize > 0 && total > expectedSize) {
                result.status = ExtractStatus::LimitExceeded;
                result.detail = "文件超过清单声明的大小: " + relative;
                failed = true;
                writeOk = false;
                break;
            }
            hasher.Update(buffer.data(), static_cast<size_t>(n));
            if (!WriteAll(fd, buffer.data(), static_cast<size_t>(n))) {
                result.status = ExtractStatus::IoError;
                result.detail = "写入目标文件失败: " + relative;
                failed = true;
                writeOk = false;
                break;
            }
        }

        Sha256Digest digest{};
        if (writeOk) {
            if (!hasher.Final(&digest)) {
                result.status = ExtractStatus::IoError;
                result.detail = "计算摘要失败: " + relative;
                failed = true;
            }
        }
        // 发布前先让内容落盘：rename 只保证命名空间原子性，
        // 内容是否已写出由这里的 fsync 负责（不声称断电持久性）。
        if (writeOk && ::fsync(fd) != 0) {
            result.status = ExtractStatus::IoError;
            result.detail = "刷盘失败: " + relative;
            failed = true;
        }
        if (::close(fd) != 0 && !failed) {
            result.status = ExtractStatus::IoError;
            result.detail = "关闭目标文件失败: " + relative;
            failed = true;
        }
        if (failed) {
            // 半截文件必须删除，避免被误认为完整。
            ::unlink(target.c_str());
            break;
        }

        ExtractedFile extracted;
        extracted.relativePath = relative;
        extracted.size = total;
        extracted.sha256Hex = ToHex(digest);
        extracted.digestComputed = true;
        result.files[relative] = extracted;
        if (callbacks.onFileDone) callbacks.onFileDone(extracted);
    }

    archive_read_close(handle);
    archive_read_free(handle);

    // 清单要求的每个文件都必须出现过（仅在当前未失败时需要检查）。
    if (!failed) {
        for (size_t i = 0; i < spec.fileCount; ++i) {
            if (result.files.count(spec.files[i].relativePath) == 0) {
                result.status = ExtractStatus::MissingFile;
                result.detail =
                    std::string("归档缺少文件: ") + spec.files[i].relativePath;
                break;
            }
        }
    }

    if (result.status != ExtractStatus::Ok) {
        // 失败时不留半套：删除本次已写出的文件。
        // 调用方通常还会整目录清理，但解包器自身也不应留下"部分成功"的痕迹。
        for (const auto& [relative, file] : result.files) {
            ::unlink((destDir + "/" + relative).c_str());
        }
        result.files.clear();
        return result;
    }

    return result;
}

} // namespace fcitx::model_manager
