#pragma once

// 测试用的进程级 XDG 沙箱与配置目录观测工具。
//
// 目的：同一个场景连续执行多遍、或 ctest 并发执行时，都必须从干净状态开始，
// 且绝不读写用户自己的 ~/.config / ~/.local/share。

#include <cstdlib>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>

// 与 engine.cpp 一致：旧发行版只有弃用的 standardpath.h。
#if __has_include(<fcitx-utils/standardpaths.h>)
#include <fcitx-utils/standardpaths.h>
#define TEST_HAS_STANDARDPATHS
#else
#include <fcitx-utils/standardpath.h>
#endif

namespace test_support {

/// 每个测试进程独占的 XDG 沙箱。
///
/// 用 mkdtemp 生成唯一路径，并在构造时把 XDG_CONFIG_HOME / XDG_DATA_HOME /
/// XDG_CACHE_HOME 指向它，因此：
///   - 同一场景连续执行两遍不会读到上一轮写下的 voiceinput.conf；
///   - ctest 并发执行时各进程互不共享目录；
///   - 只删除自己创建的目录，不触碰用户的 ~/.config、~/.local/share。
class IsolatedXdg {
public:
    IsolatedXdg() {
        const char* tmp = std::getenv("TMPDIR");
        std::string pattern = std::string(tmp && *tmp ? tmp : "/tmp") +
                              "/voice-input-backend-switch-XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (!::mkdtemp(buffer.data())) {
            throw std::runtime_error("mkdtemp failed");
        }
        root_ = buffer.data();
        std::filesystem::create_directories(root_ + "/config/fcitx5/conf");
        std::filesystem::create_directories(root_ + "/data");
        std::filesystem::create_directories(root_ + "/cache");
        SetEnv("XDG_CONFIG_HOME", root_ + "/config");
        SetEnv("XDG_DATA_HOME", root_ + "/data");
        SetEnv("XDG_CACHE_HOME", root_ + "/cache");
    }

    ~IsolatedXdg() {
        std::error_code error;
        if (!root_.empty()) std::filesystem::remove_all(root_, error);
    }

    IsolatedXdg(const IsolatedXdg&) = delete;
    IsolatedXdg& operator=(const IsolatedXdg&) = delete;

private:
    static void SetEnv(const char* name, const std::string& value) {
        if (::setenv(name, value.c_str(), 1) != 0) {
            throw std::runtime_error(std::string("setenv failed: ") + name);
        }
    }

    std::string root_;
};

// 与 engine.cpp 使用同一套路径解析：不同 fcitx 版本的 userDirectory 前缀不同，
// 不能靠猜（曾因此让“未写文件”的断言变成空断言）。
std::string ConfigRoot() {
#ifdef TEST_HAS_STANDARDPATHS
    return fcitx::StandardPaths::global()
        .userDirectory(fcitx::StandardPathsType::Config)
        .string();
#else
    return fcitx::StandardPath::global().userDirectory(
        fcitx::StandardPath::Type::Config);
#endif
}

/// fcitx 的用户配置目录（PkgConfig）：readAsIni/safeSaveAsIni 的默认落盘位置，
/// 凭据配置文件（如 voiceinput-volcengine.conf）就在这里。
std::string UserPkgConfigDir() {
#ifdef TEST_HAS_STANDARDPATHS
    return fcitx::StandardPaths::global()
        .userDirectory(fcitx::StandardPathsType::PkgConfig)
        .string();
#else
    return fcitx::StandardPath::global().userDirectory(
        fcitx::StandardPath::Type::PkgConfig);
#endif
}

/// 文件权限位（八进制）；文件不存在或 stat 失败返回 -1。
int FilePermissions(const std::string& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) return -1;
    return static_cast<int>(info.st_mode & 0777);
}

/// 预置一个指定权限的文件，用于验证“已存在保存路径”的收紧行为。
bool SeedFileWithMode(const std::string& path, const std::string& content,
                      mode_t mode) {
    std::error_code error;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), error);
    {
        std::ofstream output(path, std::ios::trunc);
        if (!output.is_open()) return false;
        output << content;
        if (!output.good()) return false;
    }
    return ::chmod(path.c_str(), mode) == 0;
}

std::string ReadFile(const std::string& path) {
    std::ifstream input(path);
    if (!input.is_open()) return {};
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

int CountConfigFiles() {
    std::error_code error;
    int count = 0;
    auto iterator = std::filesystem::recursive_directory_iterator(ConfigRoot(), error);
    auto end = std::filesystem::recursive_directory_iterator();
    while (!error && iterator != end) {
        if (iterator->is_regular_file(error)) ++count;
        iterator.increment(error);
    }
    return count;
}

} // namespace test_support
