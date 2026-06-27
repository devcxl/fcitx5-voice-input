#include "pulse_audio_capture.h"

#include <array>

#include <fcitx-utils/log.h>
#include <pulse/error.h>

namespace fcitx {

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

    int error = 0;
    stream_ = pa_simple_new(nullptr,
                            "fcitx5-voice-input",
                            PA_STREAM_RECORD,
                            nullptr,
                            "voice input",
                            &sampleSpec,
                            nullptr,
                            nullptr,
                            &error);
    if (!stream_) {
        FCITX_ERROR() << "[voice-input:pulse] Failed to open default source: "
                      << pa_strerror(error);
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
