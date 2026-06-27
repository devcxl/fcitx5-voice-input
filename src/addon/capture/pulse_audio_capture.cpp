#include "pulse_audio_capture.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcitx-utils/log.h>
#include <pulse/error.h>

namespace fcitx {
namespace {

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

bool PulseAudioCapture::Start() {
    if (running_) return true;

    pa_sample_spec sampleSpec{};
    sampleSpec.format = PA_SAMPLE_S16LE;
    sampleSpec.rate = 16000;
    sampleSpec.channels = 1;

    std::string sourceName;
    if (!configuredSource_.empty()) {
        sourceName = configuredSource_;
        FCITX_INFO() << "[voice-input:pulse] Using configured source: " << sourceName;
    } else {
        sourceName = FindBestSourceName();
    }
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
        captureThread_->join();
        captureThread_.reset();
    }

    if (stream_) {
        pa_simple_free(stream_);
        stream_ = nullptr;
    }
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
