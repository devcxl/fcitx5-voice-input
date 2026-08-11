#include "pulse_audio_capture.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>

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

constexpr size_t kPactlLineMax = 512;

const char* ExtractSourceName(char* line) {
    char* tab = std::strchr(line, '\t');
    if (!tab) return nullptr;
    char* name = tab + 1;
    tab = std::strchr(name, '\t');
    if (tab) *tab = '\0';
    return *name ? name : nullptr;
}

bool EndsWith(const char* s, const char* suffix) {
    size_t len = std::strlen(s);
    size_t suffixLen = std::strlen(suffix);
    return len >= suffixLen && std::strcmp(s + len - suffixLen, suffix) == 0;
}

std::string FindBestSourceName() {
    FILE* fp = popen("pactl list sources short 2>/dev/null", "r");
    if (!fp) {
        FCITX_WARN() << "[voice-input:pulse] Failed to run pactl";
        return "";
    }

    std::string bestSource;
    char line[kPactlLineMax];

    while (fgets(line, sizeof(line), fp)) {
        const char* name = ExtractSourceName(line);
        if (!name) continue;
        if (EndsWith(name, ".monitor")) continue;
        if (std::strstr(name, "echoCancel")) continue;

        if (std::strncmp(name, "alsa_input.", sizeof("alsa_input.") - 1) == 0) {
            if (bestSource.empty()) bestSource = name;
            if (std::strstr(name, "Mic1")) { bestSource = name; break; }
        }
        if (bestSource.empty()) bestSource = name;
    }

    int exitCode = pclose(fp);
    if (exitCode != 0) {
        FCITX_WARN() << "[voice-input:pulse] pactl exited with code " << exitCode;
    }

    if (!bestSource.empty()) {
        FCITX_INFO() << "[voice-input:pulse] Auto-selected source: " << bestSource;
    }
    return bestSource;
}

} // namespace

PulseAudioCapture::PulseAudioCapture() = default;

PulseAudioCapture::~PulseAudioCapture() { Stop(); }

// dlopen 加载 libpulse-simple 并校验关键符号。
// 返回 true 后所有 pa_* 调用均安全；失败则本后端不可用（Start 返回 false，
// 由 pipeline 回退到其他后端），不影响 addon 本体加载。
bool PulseAudioCapture::LoadLib() {
    if (pulseLib_) return true;

    for (const char* soname : kPulseLibCandidates) {
        void* handle = dlopen(soname, RTLD_NOW | RTLD_GLOBAL);
        if (!handle) continue;

        // 符号完整性校验：同名 soname 可能被错误实现/损坏库占用，
        // 必须确认入口符号真实存在，否则后续调用会 undefined symbol 崩溃
        if (!dlsym(handle, "pa_simple_new")) {
            FCITX_WARN() << "[voice-input:pulse] " << soname
                         << " loaded but pa_simple_new missing: " << dlerror();
            dlclose(handle);
            continue;
        }

        pulseLib_ = handle;
        FCITX_INFO() << "[voice-input:pulse] Runtime-loaded " << soname;
        return true;
    }

    FCITX_WARN() << "[voice-input:pulse] Failed to dlopen libpulse-simple: "
                 << dlerror();
    return false;
}

void PulseAudioCapture::UnloadLib() {
    if (pulseLib_) {
        dlclose(pulseLib_);
        pulseLib_ = nullptr;
    }
}

bool PulseAudioCapture::Start() {
    if (running_) return true;

    if (!LoadLib()) return false;

    pa_sample_spec sampleSpec{};
    sampleSpec.format = PA_SAMPLE_S16LE;
    sampleSpec.rate = 16000;
    sampleSpec.channels = 1;

    std::string sourceName = FindBestSourceName();
    const char* device = sourceName.empty() ? nullptr : sourceName.c_str();

    int error = 0;
    stream_ = pa_simple_new(nullptr,
                            "fcitx5-voice-input",
                            PA_STREAM_RECORD,
                            device,
                            "voice input",
                            &sampleSpec,
                            nullptr,
                            nullptr,
                            &error);
    if (!stream_) {
        FCITX_ERROR() << "[voice-input:pulse] Failed to open source: "
                      << (device ? device : "(default)")
                      << " — " << pa_strerror(error);
        UnloadLib();
        return false;
    }

    running_ = true;
    captureThread_ = std::make_unique<std::thread>(
        &PulseAudioCapture::CaptureLoop, this);
    FCITX_INFO() << "[voice-input:pulse] Capture started (16kHz mono int16, "
                 << kWindowSize << " sample frames)";
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
        pa_simple_free(stream_);
        stream_ = nullptr;
    }
    UnloadLib();
    FCITX_INFO() << "[voice-input:pulse] Capture stopped";
}

void PulseAudioCapture::CaptureLoop() {
    std::array<int16_t, kWindowSize> buffer{};

    while (running_) {
        int error = 0;
        if (pa_simple_read(stream_, buffer.data(),
                           buffer.size() * sizeof(int16_t), &error) < 0) {
            if (running_) {
                FCITX_ERROR() << "[voice-input:pulse] Read failed: "
                              << pa_strerror(error);
            }
            running_ = false;
            break;
        }

        if (!frameQueue_) continue;

        AudioFrame frame;
        frame.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
        frame.pcm = buffer;
        frameQueue_->Push(frame);
    }
}

} // namespace fcitx
