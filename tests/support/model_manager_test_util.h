#pragma once

// model_manager 测试共用的最小夹具（不依赖任何测试框架）。
//
// 只放：断言计数、临时目录、环境变量守卫、libarchive 写包工具。
// 具体用例分布见 tests/model_manager_test.cpp 与 tests/model_manager_extract_test.cpp。

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include <archive.h>
#include <archive_entry.h>

#include "model_catalog.h"

namespace model_manager_test {

namespace fs = std::filesystem;
using namespace fcitx::model_manager;

inline int& Failures() {
    static int failures = 0;
    return failures;
}

inline void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << "\n";
        ++Failures();
    }
}

inline int Report(const char* name) {
    if (Failures() == 0) {
        std::cout << name << ": all checks passed\n";
        return 0;
    }
    std::cerr << name << ": " << Failures() << " check(s) failed\n";
    return 1;
}

/// 每个用例独占一个临时目录，析构即清理。
struct TempDir {
    fs::path path;

    TempDir() {
        path = fs::temp_directory_path() /
               ("voice-input-mm-test-" + std::to_string(::getpid()) + "-" +
                std::to_string(++Counter()));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

private:
    static int& Counter() {
        static int counter = 0;
        return counter;
    }
};

/// 环境变量保存/恢复，避免用例之间互相污染。
class EnvGuard {
public:
    EnvGuard(std::initializer_list<const char*> names) : names_(names) {
        for (const char* name : names_) {
            if (const char* v = std::getenv(name)) {
                saved_.emplace_back(name, std::string(v));
            } else {
                saved_.emplace_back(name, std::optional<std::string>{});
            }
        }
    }

    ~EnvGuard() {
        for (const auto& [name, value] : saved_) {
            if (value) {
                ::setenv(name.c_str(), value->c_str(), 1);
            } else {
                ::unsetenv(name.c_str());
            }
        }
    }

private:
    std::vector<const char*> names_;
    std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

inline void WriteText(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

inline std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/// 用 libarchive 写一个 tar.bz2 条目。
struct TarEntry {
    std::string name;
    std::string content;
    char type = '0'; // '0' regular, '1' hardlink, '2' symlink, '5' dir
    std::string link;
};

inline bool WriteTar(const fs::path& path, const std::vector<TarEntry>& entries) {
    archive* handle = archive_write_new();
    if (!handle) return false;
    archive_write_set_format_pax_restricted(handle);
    archive_write_add_filter_bzip2(handle);
    if (archive_write_open_filename(handle, path.c_str()) != ARCHIVE_OK) {
        archive_write_free(handle);
        return false;
    }

    bool ok = true;
    for (const TarEntry& entry : entries) {
        archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, entry.name.c_str());
        archive_entry_set_perm(e, 0644);
        if (entry.type == '5') {
            archive_entry_set_filetype(e, AE_IFDIR);
            archive_entry_set_size(e, 0);
        } else if (entry.type == '2') {
            archive_entry_set_filetype(e, AE_IFLNK);
            archive_entry_set_size(e, 0);
            archive_entry_set_symlink(e, entry.link.c_str());
        } else if (entry.type == '1') {
            archive_entry_set_filetype(e, AE_IFREG);
            archive_entry_set_size(e, 0);
            archive_entry_set_hardlink(e, entry.link.c_str());
        } else {
            archive_entry_set_filetype(e, AE_IFREG);
            archive_entry_set_size(
                e, static_cast<la_int64_t>(entry.content.size()));
        }
        if (archive_write_header(handle, e) != ARCHIVE_OK) {
            ok = false;
        } else if (entry.type == '0' && !entry.content.empty()) {
            if (archive_write_data(handle, entry.content.data(),
                                   entry.content.size()) < 0) {
                ok = false;
            }
        }
        archive_entry_free(e);
        if (!ok) break;
    }

    archive_write_close(handle);
    archive_write_free(handle);
    return ok;
}

/// 造一份"文件名齐全但内容不符"的最小归档（用于走完解包路径）。
inline std::vector<TarEntry> MinimalEntriesFor(const ModelSpec& spec,
                                               const std::string& content = "x") {
    std::vector<TarEntry> entries;
    const std::string dir = std::string(spec.version) + "/";
    entries.push_back({dir, "", '5', ""});
    for (size_t i = 0; i < spec.fileCount; ++i) {
        entries.push_back({dir + spec.files[i].relativePath, content, '0', ""});
    }
    return entries;
}

} // namespace model_manager_test
