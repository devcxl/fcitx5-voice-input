#include "pulse_audio_capture.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcitx-utils/log.h>
#include <pulse/error.h>

namespace fcitx {
namespace {

constexpr size_t kPactlLineMax = 512;

// Extract source name from pactl output line.
// Format: <index>\t<name>\t<driver>\t<sample_spec>\t<state>
// Returns nullptr on parse failure.
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

// Enumerate PulseAudio sources via pactl and return the best non-monitor
// hardware microphone source name. Returns empty to use default source.
std::string FindBestSourceName() {
    FILE* fp = popen("pactl list sources short 2>/dev/null", "r");
    if (!fp) {
        FCITX_WARN() << "[voice-input:pulse] Failed to run pactl for source enumeration";
        return "";
    }

    std::string bestSource;
    char line[kPactlLineMax];

    while (fgets(line, sizeof(line), fp)) {
        const char* name = ExtractSourceName(line);
        if (!name) continue;

        // Skip monitor (loopback) sources
        if (EndsWith(name, ".monitor")) continue;
        // Skip echo-cancel virtual sources (never produce usable audio)
        if (std::strstr(name, "echoCancel")) continue;

        // Prefer ALSA hardware input sources
        if (std::strncmp(name, "alsa_input.", sizeof("alsa_input.") - 1) == 0) {
            bestSource = name;
            break;
        }

        // First non-monitor source as fallback
        if (bestSource.empty()) {
            bestSource = name;
        }
    }

    int exitCode = pclose(fp);
    if (exitCode != 0) {
        FCITX_WARN() << "[voice-input:pulse] pactl exited with code " << exitCode;
    }

    if (!bestSource.empty()) {
        FCITX_INFO() << "[voice-input:pulse] Auto-selected source: " << bestSource;
    } else {
        FCITX_INFO() << "[voice-input:pulse] No suitable source found, using default";
    }

    return bestSource;
}

} // anonymous namespace

PulseAudioCapture::PulseAudioCapture()
    : ringBuffer_(std::make_unique<AudioRingBuffer>(65536)) {}

PulseAudioCapture::~PulseAudioCapture() { Stop(); }

bool PulseAudioCapture::Start() {
    if (running_) {
        return true;
    }

    pa_sample_spec sampleSpec{};
    sampleSpec.format = PA_SAMPLE_FLOAT32LE;
    sampleSpec.rate = 16000;
    sampleSpec.channels = 1;

    // Auto-detect best microphone source, fall back to default
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
        return false;
    }

    running_ = true;
    captureThread_ = std::make_unique<std::thread>(&PulseAudioCapture::CaptureLoop, this);
    FCITX_INFO() << "[voice-input:pulse] PulseAudio capture started successfully";
    return true;
}

void PulseAudioCapture::Stop() {
    if (!running_ && !stream_) {
        return;
    }

    running_ = false;
    if (captureThread_ && captureThread_->joinable()) {
        captureThread_->join();
        captureThread_.reset();
    }

    if (stream_) {
        pa_simple_free(stream_);
        stream_ = nullptr;
    }
    FCITX_INFO() << "[voice-input:pulse] PulseAudio capture stopped";
}

void PulseAudioCapture::CaptureLoop() {
    std::array<float, 320> buffer{};

    while (running_) {
        int error = 0;
        if (pa_simple_read(stream_, buffer.data(), buffer.size() * sizeof(float), &error) < 0) {
            if (running_) {
                FCITX_ERROR() << "[voice-input:pulse] Read failed: " << pa_strerror(error);
            }
            running_ = false;
            break;
        }
        ringBuffer_->Write(buffer.data(), buffer.size());
    }
}

} // namespace fcitx
