#include "pulse_audio_capture.h"

#include <array>
#include <chrono>
#include <dlfcn.h>

#include <fcitx-utils/log.h>
#include <pulse/error.h>

namespace fcitx {
namespace {

// libpulse-simple 运行时库候选 soname（按优先级）。当前所有主流发行版为 .so.0；
// 未来若 soname 变更，旧版本 addon 仍可尝试后续候选，任一成功即可用。
constexpr const char* kPulseLibCandidates[] = {
    "libpulse-simple.so.0",
    "libpulse-simple.so.1",
};

} // namespace

PulseAudioCapture::PulseAudioCapture() = default;

PulseAudioCapture::~PulseAudioCapture() { Stop(); }

// dlopen 加载 libpulse-simple 并通过 dlsym 填充函数指针表。
// 返回 true 后所有 pa_* 调用均安全（经 pulse_ 间接调用，无未定义符号）；
// 失败则本后端不可用（Start 返回 false，由 pipeline 回退到其他后端），
// 不影响 addon 本体加载。
bool PulseAudioCapture::LoadLib() {
    if (pulse_.handle) return true;

    for (const char* soname : kPulseLibCandidates) {
        void* handle = dlopen(soname, RTLD_NOW | RTLD_GLOBAL);
        if (!handle) continue;

        PulseLib lib{};
        lib.handle = handle;
        const char* missingSymbol = nullptr;
#define PULSE_LOAD(sym)                                                               \
        do {                                                                          \
            lib.sym = reinterpret_cast<decltype(lib.sym)>(dlsym(handle, #sym));     \
            if (!lib.sym && !missingSymbol) {                                        \
                missingSymbol = #sym;                                                \
            }                                                                         \
        } while (false)
        PULSE_LOAD(pa_simple_new);
        PULSE_LOAD(pa_simple_free);
        PULSE_LOAD(pa_simple_read);
        PULSE_LOAD(pa_strerror);
#undef PULSE_LOAD

        // 符号完整性校验：同名 soname 可能被错误实现/损坏库占用，
        // 任一入口缺失即拒绝，避免后续调用空指针崩溃。
        if (!lib.valid()) {
            FCITX_WARN() << "[voice-input:pulse] " << soname
                         << " loaded but required symbol missing: "
                         << (missingSymbol ? missingSymbol : "unknown");
            dlclose(handle);
            continue;
        }

        pulse_ = lib;
        FCITX_INFO() << "[voice-input:pulse] Runtime-loaded " << soname;
        return true;
    }

    FCITX_WARN() << "[voice-input:pulse] Failed to dlopen libpulse-simple: "
                 << dlerror();
    return false;
}

void PulseAudioCapture::UnloadLib() {
    if (pulse_.handle) {
        dlclose(pulse_.handle);
        pulse_ = PulseLib{};
    }
}

bool PulseAudioCapture::Start() {
    if (running_) return true;

    if (!LoadLib()) return false;

    pa_sample_spec sampleSpec{};
    sampleSpec.format = PA_SAMPLE_S16LE;
    sampleSpec.rate = 16000;
    sampleSpec.channels = 1;

    int error = 0;
    // dev = nullptr：不指定物理录音源，把路由委托给音频服务与音频管理器。
    // 它们负责默认源、按应用偏好与运行中移流；应用身份（应用名/流名）保持不变，
    // 以免破坏已保存的应用设备偏好（issue #54）。
    stream_ = pulse_.pa_simple_new(nullptr,
                                   "fcitx5-voice-input",
                                   PA_STREAM_RECORD,
                                   nullptr,
                                   "voice input",
                                   &sampleSpec,
                                   nullptr,
                                   nullptr,
                                   &error);
    if (!stream_) {
        FCITX_ERROR() << "[voice-input:pulse] Failed to open capture stream: "
                      << pulse_.pa_strerror(error);
        UnloadLib();
        return false;
    }

    running_ = true;
    captureThread_ = std::make_unique<std::thread>(
        &PulseAudioCapture::CaptureLoop, this);
    FCITX_INFO() << "[voice-input:pulse] Capture started (16kHz mono int16, "
                 << kWindowSize
                 << " sample frames; routing managed by the audio service)";
    return true;
}

void PulseAudioCapture::Stop() {
    if (!running_ && !stream_) return;

    running_ = false;
    if (captureThread_ && captureThread_->joinable()) {
        // pa_simple_read 是同步阻塞调用，只能等线程退出后再释放流。
        // 严禁在 join 前并发调用 pa_simple_free()：libpulse 对象不支持跨
        // 线程并发访问，free 与阻塞中的 read 并发会构成 use-after-free/
        // 堆损坏（曾引发 free(): invalid pointer 崩溃，见 PR #17）。
        // 设备挂起时 Stop 可能因此阻塞——根治需改用 async API，见 issue。
        captureThread_->join();
        captureThread_.reset();
    }

    if (stream_) {
        pulse_.pa_simple_free(stream_);
        stream_ = nullptr;
    }
    UnloadLib();
    FCITX_INFO() << "[voice-input:pulse] Capture stopped";
}

void PulseAudioCapture::CaptureLoop() {
    std::array<int16_t, kWindowSize> buffer{};

    while (running_) {
        const auto epoch = DeliveryEpoch();
        int error = 0;
        if (pulse_.pa_simple_read(stream_, buffer.data(),
                                  buffer.size() * sizeof(int16_t), &error) < 0) {
            if (running_) {
                FCITX_ERROR() << "[voice-input:pulse] Read failed: "
                              << pulse_.pa_strerror(error);
            }
            running_ = false;
            break;
        }

        if (!running_) break;

        AudioFrame frame;
        frame.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
        frame.pcm = buffer;
        DeliverFrame(std::move(frame), epoch);
    }
}

} // namespace fcitx
