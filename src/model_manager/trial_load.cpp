#include "trial_load.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/// 以及定位与安装器一同部署的 sherpa 运行库）。
std::string SelfExePath() {
    char buffer[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n <= 0) return {};
    buffer[n] = '\0';
    return buffer;
}

std::string DirName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    return slash == 0 ? "/" : path.substr(0, slash);
}

#if defined(HAVE_SHERPA_ONNX)
#include <sherpa-onnx/c-api/c-api.h>
#endif

namespace fcitx::model_manager {

namespace {

using Clock = std::chrono::steady_clock;

uint64_t ElapsedMs(Clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                             start)
            .count());
}

/// stderr 尾部保留上限：只用于诊断，不保留全部输出。
constexpr size_t kStderrTail = 4096;

/// 可执行文件自身路径（用于 spawn 自身的 --verify-internal 模式，
/// 以及定位与安装器一同部署的 sherpa 运行库）。
/// 与是否编入 sherpa 无关，必须始终可用。
std::string SelfExePath() {
    char buffer[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n <= 0) return {};
    buffer[n] = '\0';
    return buffer;
}

#if defined(HAVE_SHERPA_ONNX)

/// 路径的父目录（无 '/' 时返回 "."，根目录返回 "/"）。仅用于定位运行库。
std::string DirName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    return slash == 0 ? "/" : path.substr(0, slash);
}

/// sherpa 运行库的候选路径。安装器不链接它（dlopen 延迟加载），
/// 因此库缺失时试加载报告 unavailable 而不是启动失败。
///
/// 搜索顺序：
///   1. $VOICE_INPUT_SHERPA_LIB（显式覆盖，便于测试与非标准安装）；
///   2. 可执行文件同目录（安装器可与 sherpa 一起部署，$ORIGIN 语义）；
///   3. <exe>/../lib/fcitx5、<exe>/../lib64/fcitx5（安装器在 bin/ 而 addon
///      与 sherpa 在 lib/fcitx5/ 的常见布局）；
///   4. 裸 soname，交给动态链接器的默认搜索路径（ldconfig 已登记的安装）。
///
/// 注意：从不搜索当前工作目录，避免从 cwd 加载同名恶意库。
std::vector<std::string> SherpaLibraryCandidates() {
    std::vector<std::string> out;
    if (const char* env = std::getenv("VOICE_INPUT_SHERPA_LIB"); env && *env) {
        out.emplace_back(env);
    }

    const std::string exe = SelfExePath();
    if (!exe.empty()) {
        const std::string dir = DirName(exe);
        for (const char* leaf : {"libsherpa-onnx-c-api.so.1",
                                 "libsherpa-onnx-c-api.so"}) {
            out.push_back(dir + "/" + leaf);
        }
        for (const char* sub : {"/../lib/fcitx5/", "/../lib64/fcitx5/",
                                "/../lib/", "/../lib64/"}) {
            for (const char* leaf : {"libsherpa-onnx-c-api.so.1",
                                     "libsherpa-onnx-c-api.so"}) {
                out.push_back(dir + sub + leaf);
            }
        }
    }

    out.emplace_back("libsherpa-onnx-c-api.so.1");
    out.emplace_back("libsherpa-onnx-c-api.so");
    return out;
}

/// 函数指针表。用 decltype(&fn) 取得精确签名（不求值，因此不产生未定义符号），
/// 结构体布局仍由上游头文件保证。
struct SherpaApi {
    void* handle = nullptr;
    decltype(&SherpaOnnxCreateOnlineRecognizer) createOnline = nullptr;
    decltype(&SherpaOnnxDestroyOnlineRecognizer) destroyOnline = nullptr;
    decltype(&SherpaOnnxCreateOfflineRecognizer) createOffline = nullptr;
    decltype(&SherpaOnnxDestroyOfflineRecognizer) destroyOffline = nullptr;
};

bool LoadSherpa(SherpaApi* api) {
    void* handle = nullptr;
    for (const std::string& name : SherpaLibraryCandidates()) {
        handle = ::dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle) break;
    }
    if (!handle) return false;

    api->createOnline = reinterpret_cast<decltype(api->createOnline)>(
        ::dlsym(handle, "SherpaOnnxCreateOnlineRecognizer"));
    api->destroyOnline = reinterpret_cast<decltype(api->destroyOnline)>(
        ::dlsym(handle, "SherpaOnnxDestroyOnlineRecognizer"));
    api->createOffline = reinterpret_cast<decltype(api->createOffline)>(
        ::dlsym(handle, "SherpaOnnxCreateOfflineRecognizer"));
    api->destroyOffline = reinterpret_cast<decltype(api->destroyOffline)>(
        ::dlsym(handle, "SherpaOnnxDestroyOfflineRecognizer"));

    const bool complete = api->createOnline && api->destroyOnline &&
                          api->createOffline && api->destroyOffline;
    if (!complete) {
        ::dlclose(handle);
        *api = SherpaApi{};
        return false;
    }
    api->handle = handle;
    return true;
}

/// 构造一次在线 recognizer。失败返回 false；致命失败会让 sherpa 直接 _Exit，
/// 这正是本函数必须跑在子进程里的原因。
bool TryOnline(const ModelSpec& spec, const std::string& versionDir,
               const SherpaApi& api) {
    const ModelFileSpec* encoder = FindModelFileByRole(spec, "encoder");
    const ModelFileSpec* decoder = FindModelFileByRole(spec, "decoder");
    const ModelFileSpec* joiner = FindModelFileByRole(spec, "joiner");
    const ModelFileSpec* tokens = FindModelFileByRole(spec, "tokens");
    if (!encoder || !decoder || !joiner || !tokens) return false;

    const std::string enc = versionDir + "/" + encoder->relativePath;
    const std::string dec = versionDir + "/" + decoder->relativePath;
    const std::string joi = versionDir + "/" + joiner->relativePath;
    const std::string tok = versionDir + "/" + tokens->relativePath;

    SherpaOnnxOnlineRecognizerConfig config;
    std::memset(&config, 0, sizeof(config));
    config.feat_config.sample_rate = 16000;
    config.feat_config.feature_dim = 80;
    config.model_config.transducer.encoder = enc.c_str();
    config.model_config.transducer.decoder = dec.c_str();
    config.model_config.transducer.joiner = joi.c_str();
    config.model_config.tokens = tok.c_str();
    config.model_config.num_threads = 1;
    config.model_config.provider = "cpu";
    config.model_config.modeling_unit = "cjkchar";
    config.decoding_method = "greedy_search";
    config.max_active_paths = 8;
    config.enable_endpoint = 0;

    const SherpaOnnxOnlineRecognizer* recognizer = api.createOnline(&config);
    if (!recognizer) return false;
    api.destroyOnline(recognizer);
    return true;
}

bool TryOffline(const ModelSpec& spec, const std::string& versionDir,
                const SherpaApi& api) {
    const ModelFileSpec* encoder = FindModelFileByRole(spec, "encoder");
    const ModelFileSpec* decoder = FindModelFileByRole(spec, "decoder");
    const ModelFileSpec* joiner = FindModelFileByRole(spec, "joiner");
    const ModelFileSpec* tokens = FindModelFileByRole(spec, "tokens");
    if (!encoder || !decoder || !joiner || !tokens) return false;

    const std::string enc = versionDir + "/" + encoder->relativePath;
    const std::string dec = versionDir + "/" + decoder->relativePath;
    const std::string joi = versionDir + "/" + joiner->relativePath;
    const std::string tok = versionDir + "/" + tokens->relativePath;

    SherpaOnnxOfflineRecognizerConfig config;
    std::memset(&config, 0, sizeof(config));
    config.feat_config.sample_rate = 16000;
    config.feat_config.feature_dim = 80;
    config.model_config.transducer.encoder = enc.c_str();
    config.model_config.transducer.decoder = dec.c_str();
    config.model_config.transducer.joiner = joi.c_str();
    config.model_config.tokens = tok.c_str();
    config.model_config.num_threads = 1;
    config.model_config.provider = "cpu";
    config.model_config.modeling_unit = "cjkchar";
    // bpe_vocab 必须留空：传 bpe.model（sentencepiece 二进制 proto）会让
    // Ssentencepiece::LoadVocab 解析失败并 exit(-1)（见 ADR-0005）。
    config.decoding_method = "greedy_search";
    config.max_active_paths = 8;

    const SherpaOnnxOfflineRecognizer* recognizer = api.createOffline(&config);
    if (!recognizer) return false;
    api.destroyOffline(recognizer);
    return true;
}

#endif // HAVE_SHERPA_ONNX

} // namespace

const char* TrialStatusName(TrialStatus status) {
    switch (status) {
        case TrialStatus::Ok: return "ok";
        case TrialStatus::Failed: return "failed";
        case TrialStatus::Timeout: return "timeout";
        case TrialStatus::Cancelled: return "cancelled";
        case TrialStatus::SpawnFailed: return "spawn_failed";
        case TrialStatus::Unavailable: return "unavailable";
    }
    return "failed";
}

int RunVerifyInternal(const ModelSpec& spec, const std::string& versionDir) {
#if !defined(HAVE_SHERPA_ONNX)
    (void)spec;
    (void)versionDir;
    std::fprintf(stderr, "verify-internal: 本安装器构建时未包含 sherpa\n");
    return 5;
#else
    if (versionDir.empty()) {
        std::fprintf(stderr, "verify-internal: 版本目录为空\n");
        return 2;
    }

    // 每个文件必须先确认存在且是普通文件（不跟随链接）。
    // O_NONBLOCK + fstat：路径若是 FIFO/设备，open 或后续读取可能永久阻塞，
    // 子进程虽有限时兜底，但不该靠超时才失败。
    for (size_t i = 0; i < spec.fileCount; ++i) {
        const std::string path =
            versionDir + "/" + spec.files[i].relativePath;
        const int fd = ::open(path.c_str(),
                              O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) {
            std::fprintf(stderr, "verify-internal: 无法打开 %s\n",
                         spec.files[i].relativePath);
            return 3;
        }
        struct stat st {};
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            ::close(fd);
            std::fprintf(stderr, "verify-internal: %s 不是普通文件\n",
                         spec.files[i].relativePath);
            return 3;
        }
        ::close(fd);
    }

    SherpaApi api;
    if (!LoadSherpa(&api)) {
        std::fprintf(stderr, "verify-internal: 找不到 sherpa 运行库\n");
        return 5;
    }

    const bool ok = spec.streaming ? TryOnline(spec, versionDir, api)
                                   : TryOffline(spec, versionDir, api);
    if (api.handle) ::dlclose(api.handle);
    if (!ok) {
        std::fprintf(stderr, "verify-internal: 引擎拒绝了这组模型文件\n");
        return 4;
    }
    std::fprintf(stderr, "verify-internal: ok\n");
    return 0;
#endif
}

TrialResult RunBoundedChild(const std::vector<std::string>& argv,
                            const TrialOptions& options) {
    const Clock::time_point start = Clock::now();
    TrialResult result;

    if (argv.empty() || argv.front().empty()) {
        result.status = TrialStatus::SpawnFailed;
        result.stderrTail = "子进程参数为空";
        return result;
    }

    int errPipe[2] = {-1, -1};
    if (::pipe2(errPipe, O_CLOEXEC) != 0) {
        result.status = TrialStatus::SpawnFailed;
        result.stderrTail = std::string("pipe2 失败: ") + std::strerror(errno);
        return result;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(errPipe[0]);
        ::close(errPipe[1]);
        result.status = TrialStatus::SpawnFailed;
        result.stderrTail = std::string("fork 失败: ") + std::strerror(errno);
        return result;
    }

    if (pid == 0) {
        // 子进程：把自己挂到自己的进程组，父进程可整组收割，避免留下孤儿。
        ::setpgid(0, 0);
        ::dup2(errPipe[1], STDERR_FILENO);
        // 协议 stdout 必须干净：子进程的诊断只走 stderr（已重定向）。
        const int devNull = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devNull >= 0) {
            ::dup2(devNull, STDOUT_FILENO);
            ::close(devNull);
        }
        ::close(errPipe[0]);
        ::close(errPipe[1]);

        std::vector<char*> raw;
        raw.reserve(argv.size() + 1);
        for (const auto& arg : argv) raw.push_back(const_cast<char*>(arg.c_str()));
        raw.push_back(nullptr);
        ::execv(argv.front().c_str(), raw.data());
        // exec 失败：写 stderr 后以专用码退出。
        std::fprintf(stderr, "verify-internal: execv 失败: %s\n",
                     std::strerror(errno));
        ::_exit(127);
    }

    // 父进程。
    ::close(errPipe[1]);
    ::setpgid(pid, pid);

    std::string tail;
    bool exited = false;
    int waitStatus = 0;
    bool killedForTimeout = false;
    bool killedForCancel = false;

    while (true) {
        // 排空 stderr，避免子进程因管道写满而阻塞（否则会误判超时）。
        struct pollfd pfd = {errPipe[0], POLLIN, 0};
        const int ready = ::poll(&pfd, 1, 0);
        if (ready > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            char buffer[1024];
            const ssize_t n = ::read(errPipe[0], buffer, sizeof(buffer));
            if (n > 0) {
                tail.append(buffer, static_cast<size_t>(n));
                if (tail.size() > kStderrTail) {
                    tail.erase(0, tail.size() - kStderrTail);
                }
            } else if (n == 0) {
                // EOF：子进程已关闭 stderr。
            }
        }

        const pid_t waited = ::waitpid(pid, &waitStatus, WNOHANG);
        if (waited == pid) {
            exited = true;
            break;
        }
        if (waited < 0 && errno != EINTR) {
            result.status = TrialStatus::SpawnFailed;
            result.stderrTail = std::string("waitpid 失败: ") + std::strerror(errno);
            ::close(errPipe[0]);
            return result;
        }

        if (options.cancelled && options.cancelled()) {
            killedForCancel = true;
            break;
        }
        if (options.timeoutMs > 0 && ElapsedMs(start) >= options.timeoutMs) {
            killedForTimeout = true;
            break;
        }
        ::poll(nullptr, 0, 20);
    }

    if (!exited) {
        // 整组 SIGKILL：子进程自己可能再派生子进程，只杀主进程会留下孤儿。
        ::kill(-pid, SIGKILL);
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &waitStatus, 0);
    } else {
        // 直接子进程已退出，但它派生的后代可能仍持有 stderr 管道。
        // 先清掉整个进程组，否则下面的收尾读取会一直等到 EOF（= 后代退出），
        // 从而绕过超时。忽略失败：组内可能已无成员。
        ::kill(-pid, SIGKILL);
    }

    // 收尾：**有界**地读掉 stderr 残留，绝不无限等待。
    // 即便还有存活的后代持有管道写端，也只会读取当前已缓冲的数据然后关闭。
    char buffer[1024];
    int drainRounds = 0;
    while (drainRounds++ < 8) {
        struct pollfd pfd = {errPipe[0], POLLIN, 0};
        const int ready = ::poll(&pfd, 1, 0);
        if (ready <= 0) break;
        const ssize_t n = ::read(errPipe[0], buffer, sizeof(buffer));
        if (n <= 0) break;
        tail.append(buffer, static_cast<size_t>(n));
        if (tail.size() > kStderrTail) {
            tail.erase(0, tail.size() - kStderrTail);
        }
    }
    ::close(errPipe[0]);

    result.elapsedMs = ElapsedMs(start);
    result.stderrTail = tail;

    if (killedForCancel) {
        result.status = TrialStatus::Cancelled;
        return result;
    }
    if (killedForTimeout) {
        result.status = TrialStatus::Timeout;
        return result;
    }

    if (WIFSIGNALED(waitStatus)) {
        // 段错误 / 被 OOM killer 杀死都走这里：这是子进程隔离的核心收益。
        result.status = TrialStatus::Failed;
        result.exitCode = -1;
        result.termSignal = WTERMSIG(waitStatus);
        return result;
    }

    if (WIFEXITED(waitStatus)) {
        const int code = WEXITSTATUS(waitStatus);
        result.exitCode = code;
        if (code == 0) {
            result.status = TrialStatus::Ok;
        } else if (code == 5) {
            result.status = TrialStatus::Unavailable;
        } else {
            result.status = TrialStatus::Failed;
        }
        return result;
    }

    result.status = TrialStatus::Failed;
    return result;
}

TrialResult RunTrialLoad(const ModelSpec& spec, const std::string& versionDir,
                         const TrialOptions& options) {
    const std::string self = SelfExePath();
    if (self.empty()) {
        TrialResult result;
        result.status = TrialStatus::SpawnFailed;
        result.stderrTail = "无法确定安装器自身路径";
        return result;
    }
    return RunBoundedChild({self, "--verify-internal", spec.id, versionDir},
                           options);
}

} // namespace fcitx::model_manager
