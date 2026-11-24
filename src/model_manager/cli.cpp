#include "cli.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "install_events.h"
#include "installed_model.h"
#include "model_catalog.h"
#include "model_installer.h"
#include "model_paths.h"
#include "trial_load.h"

namespace fcitx::model_manager {

namespace {

/// 信号标志：只在信号处理函数里置位，实际清理在主流程中完成。
/// sig_atomic_t 之外的原子量用 volatile sig_atomic_t 保存，避免信号安全问题。
volatile sig_atomic_t g_cancelRequested = 0;
volatile sig_atomic_t g_cancelSignal = 0;

extern "C" void OnTerminationSignal(int signo) {
    g_cancelRequested = 1;
    g_cancelSignal = signo;
}

/// 安装期间忽略 SIGPIPE：GUI 关闭管道时不应杀死安装器，
/// 而是让 stdout 写入失败后由主流程按取消处理。
void InstallSignalHandlers() {
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = OnTerminationSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0; // 不设 SA_RESTART，让阻塞的 I/O 被中断以便尽快响应
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);
    ::sigaction(SIGHUP, &action, nullptr);

    struct sigaction ignore;
    std::memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    ::sigaction(SIGPIPE, &ignore, nullptr);
}

/// 把失败映射到稳定退出码（GUI 可据此分支，但首选 result 事件）。
int ExitCodeFor(InstallError error) {
    switch (error) {
        case InstallError::None: return 0;
        case InstallError::Usage: return 2;
        case InstallError::UnknownModel: return 3;
        case InstallError::LockBusy: return 4;
        case InstallError::Cancelled: return 5;
        case InstallError::ArchiveMismatch: return 6;
        case InstallError::ArchiveUnsafe: return 7;
        case InstallError::FileMismatch: return 8;
        case InstallError::MissingFile: return 9;
        case InstallError::TrialLoadFailed: return 10;
        case InstallError::TrialLoadTimeout: return 11;
        case InstallError::DownloadFailed: return 12;
        case InstallError::ExtractFailed: return 13;
        case InstallError::PublishFailed: return 14;
        case InstallError::UnsafePath: return 15;
        case InstallError::Internal: return 16;
    }
    return 16;
}

void PrintUsage(std::FILE* out) {
    std::fprintf(out,
                 "用法: voice-input-model-installer --model <id> [选项]\n"
                 "\n"
                 "  --model <id>            要安装的模型：online-zipformer 或 offline-x-asr\n"
                 "  --archive <path>        使用本地归档安装（不联网）\n"
                 "  --trial-timeout-ms <n>  试加载超时，默认 120000\n"
                 "  --check-model <id>      只读查询安装状态，输出单行 JSON\n"
                 "  --list                  列出受控模型并退出\n"
                 "  --verify-internal <id> <dir>  内部使用：在子进程中试加载\n"
                 "\n"
                 "试加载不可跳过：它是发布前的必要验收，失败即不发布。\n");
}

/// 供 --list 使用：输出一行 JSON 描述所有受控模型。
void EmitModelList() {
    size_t count = 0;
    const ModelSpec* specs = ListModelSpecs(&count);
    for (size_t i = 0; i < count; ++i) {
        const ModelSpec& s = specs[i];
        std::printf(
            "{\"event\":\"model\",\"model\":\"%s\",\"version\":\"%s\","
            "\"backend\":\"%s\",\"streaming\":%s,\"archiveSize\":%llu,"
            "\"files\":%zu}\n",
            s.id, s.version, s.backendId, s.streaming ? "true" : "false",
            static_cast<unsigned long long>(s.archiveSize), s.fileCount);
    }
    std::fflush(stdout);
}

} // namespace

int RunInstallerCli(int argc, char** argv) {
    // --verify-internal 是子进程模式：不安装信号处理、不写协议，
    // 也不打印任何非诊断信息到 stdout（stdout 已由父进程重定向到 /dev/null）。
    if (argc >= 2 && std::strcmp(argv[1], "--verify-internal") == 0) {
        if (argc < 4) {
            std::fprintf(stderr, "verify-internal: 参数不足\n");
            return 2;
        }
        const ModelSpec* spec = FindModelSpec(argv[2]);
        if (!spec) {
            std::fprintf(stderr, "verify-internal: 未知模型 %s\n", argv[2]);
            return 2;
        }
        return RunVerifyInternal(*spec, argv[3]);
    }

    InstallSignalHandlers();

    // ── 只读状态查询：--check-model <id> ─────────────────────────────────
    //
    // 语义（GUI 依赖）：
    //   - 单行 JSON：{"event":"installed_status","model":<id>,"state":<...>}
    //   - state ∈ absent | valid | tampered | unreadable（内置清单逐文件摘要判定）
    //   - **只读**：不创建目录、不写文件、不联网；哈希消耗在本进程外运行
    //   - 退出码 0 表示查询完成（无论 state 是什么）；
    //     未知模型返回 3（不输出状态行，避免把未知 id 误报成 absent）。
    if (argc >= 2 && std::strcmp(argv[1], "--check-model") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "--check-model 需要模型 id\n");
            return 2;
        }
        const char* requested = argv[2];
        const ModelSpec* spec = FindModelSpec(requested);
        if (!spec) {
            std::fprintf(stderr, "未知模型: %s\n", requested);
            return 3;
        }

        const std::string versionDir = VersionDir(spec->id, spec->version);
        if (versionDir.empty()) {
            std::fprintf(stderr, "无法确定模型目录（XDG_DATA_HOME/HOME）\n");
            return 16;
        }

        const InstalledCheck check = CheckInstalledModel(
            *spec, versionDir, [] { return g_cancelRequested != 0; });
        if (check.cancelled) {
            // 查询被中断：不输出状态行（避免把"未查完"当成某个具体状态）。
            std::fprintf(stderr, "状态查询已取消\n");
            return 5;
        }
        if (!EmitInstalledStatus(*spec, InstalledStateName(check.state))) {
            // 管道已关闭：查询本身已完成，按成功返回即可。
            return 0;
        }
        return 0;
    }

    std::string modelId;
    std::string archivePath;
    uint32_t trialTimeoutMs = 120000;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        auto needsValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 %s 缺少取值\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (std::strcmp(arg, "--model") == 0) {
            const char* value = needsValue(arg);
            if (!value) return 2;
            modelId = value;
        } else if (std::strcmp(arg, "--archive") == 0) {
            const char* value = needsValue(arg);
            if (!value) return 2;
            archivePath = value;
        } else if (std::strcmp(arg, "--trial-timeout-ms") == 0) {
            const char* value = needsValue(arg);
            if (!value) return 2;
            char* end = nullptr;
            const long parsed = std::strtol(value, &end, 10);
            if (!end || *end != '\0' || parsed <= 0 || parsed > 3600000) {
                std::fprintf(stderr, "无效的 --trial-timeout-ms: %s\n", value);
                return 2;
            }
            trialTimeoutMs = static_cast<uint32_t>(parsed);
        } else if (std::strcmp(arg, "--list") == 0) {
            EmitModelList();
            return 0;
        } else if (std::strcmp(arg, "--help") == 0 ||
                   std::strcmp(arg, "-h") == 0) {
            PrintUsage(stdout);
            return 0;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", arg);
            PrintUsage(stderr);
            return 2;
        }
    }

    if (modelId.empty()) {
        std::fprintf(stderr, "缺少 --model\n");
        PrintUsage(stderr);
        // 参数错误也要给出协议结果，便于 GUI 统一处理。
        InstallEvent e;
        e.stage = InstallStage::Result;
        e.ok = false;
        e.error = InstallError::Usage;
        e.detail = "缺少 --model";
        EmitEvent(e);
        return 2;
    }

    const ModelSpec* spec = FindModelSpec(modelId);
    if (!spec) {
        std::fprintf(stderr, "未知模型: %s\n", modelId.c_str());
        InstallEvent e;
        e.stage = InstallStage::Result;
        e.modelId = modelId;
        e.ok = false;
        e.error = InstallError::UnknownModel;
        e.detail = "未知模型: " + modelId;
        EmitEvent(e);
        return 3;
    }

    InstallOptions options;
    options.archivePath = archivePath;
    options.trialTimeoutMs = trialTimeoutMs;
    // 生产路径始终做试加载；库 API 保留开关仅供测试使用。
    options.runTrialLoad = true;
    options.cancelled = [] { return g_cancelRequested != 0; };
    options.emit = [](const InstallEvent& event) {
        // stdout 写失败（GUI 关闭管道）不会杀进程（SIGPIPE 已忽略），
        // 但应视作取消信号，让安装尽快收敛而不是继续下载几百 MB。
        if (!EmitEvent(event)) {
            g_cancelRequested = 1;
        }
    };

    const InstallOutcome outcome = InstallModel(*spec, options);
    return ExitCodeFor(outcome.error);
}

} // namespace fcitx::model_manager
