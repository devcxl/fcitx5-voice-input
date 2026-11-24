#pragma once

// 试加载（validate by construction）：在**独立子进程**里真正构造一次 sherpa
// Recognizer，确认这组文件能被预期后端加载。
//
// 为什么必须是子进程（见 docs/development/research/sherpa-model-install-and-loading.md
// §2.2 / §4.5）：sherpa-onnx 在模型类型错配时用 SHERPA_ONNX_EXIT 调 _Exit(-1)，
// 或在 OnlineTransducerModel::Create() 返回 nullptr 后直接解引用（SIGSEGV）。
// 这两种失败都无法用 try/catch 接住，一旦发生在 addon 进程内就是输入法进程消失。
// 把试加载放进本安装器自己的子进程，致命失败就退化成"退出码非零"。
//
// 子进程复用**本二进制**的 --verify-internal 模式（不额外引入可执行文件），
// 由父进程限时等待；超时或取消时 SIGKILL，避免留下孤儿。
//
// sherpa 通过 dlopen 延迟加载：安装器不硬链接 libsherpa-onnx-c-api.so，
// 库缺失时报告 trial_unavailable 而不是加载失败。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "model_catalog.h"

namespace fcitx::model_manager {

/// 试加载结果分类。
enum class TrialStatus {
    Ok = 0,
    /// 子进程以非零码退出（模型不被该后端接受，或被信号杀死）。
    Failed,
    /// 超过时限仍未结束，已被 SIGKILL。
    Timeout,
    /// 调用方取消。
    Cancelled,
    /// 无法启动子进程本身（fork/exec 失败）。
    SpawnFailed,
    /// 找不到可用的 sherpa 运行库，无法判定。
    Unavailable,
};

const char* TrialStatusName(TrialStatus status);

struct TrialResult {
    TrialStatus status = TrialStatus::Ok;
    /// 子进程退出码（未正常退出时为 -1）。
    int exitCode = -1;
    /// 子进程被信号终止时的信号编号（否则为 0）。
    int termSignal = 0;
    /// 子进程 stderr 的尾部（已截断，可能为空）。
    std::string stderrTail;
    uint64_t elapsedMs = 0;
};

struct TrialOptions {
    /// 硬超时（毫秒）。超时即 SIGKILL。
    uint32_t timeoutMs = 120000;
    /// 每 20ms 轮询一次；返回 true 时终止子进程并返回 Cancelled。
    std::function<bool()> cancelled;
};

/// 子进程内部入口：在**当前进程**里尝试加载并返回退出码。
/// 由 main() 在识别到 --verify-internal 时调用，不应在 addon 进程内调用。
/// 退出码：0 成功；2 参数错误；3 模型文件缺失；4 加载被拒绝（不可用）；
///         5 sherpa 运行库不可用。致命信号/_Exit 会直接终止子进程。
int RunVerifyInternal(const ModelSpec& spec, const std::string& versionDir);

/// 父进程侧：spawn 自身 --verify-internal 子进程并限时等待。
TrialResult RunTrialLoad(const ModelSpec& spec, const std::string& versionDir,
                         const TrialOptions& options);

/// 限时运行任意子命令并回收**整个进程组**（供 RunTrialLoad 与测试使用）。
///
/// 保证：
///   - 超时/取消时对子进程所在进程组发 SIGKILL，并在其后再次清理该组，
///     因此子进程派生的后代不会长期存活；
///   - stderr 的读取始终是有界的：直接子进程退出后不会为了等 EOF 而阻塞，
///     即使仍有后代持有写端。stderr 只保留尾部若干字节用于诊断。
TrialResult RunBoundedChild(const std::vector<std::string>& argv,
                            const TrialOptions& options);

} // namespace fcitx::model_manager
