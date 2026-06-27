#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include <pulse/simple.h>

#include "capture/audio_capture.h"

namespace fcitx {

class PulseAudioCapture : public AudioCapture {
public:
    PulseAudioCapture();
    ~PulseAudioCapture() override;

    PulseAudioCapture(const PulseAudioCapture&) = delete;
    PulseAudioCapture& operator=(const PulseAudioCapture&) = delete;

    bool Start() override;
    void Stop() override;
    bool IsRunning() const override { return running_; }
    const char* Name() const override { return "pulseaudio"; }
    void SetSourceName(const std::string& name) override { configuredSource_ = name; }

private:
    void CaptureLoop();

    pa_simple* stream_ = nullptr;
    std::unique_ptr<std::thread> captureThread_;
    std::atomic<bool> running_{false};
    std::string configuredSource_;
};

} // namespace fcitx
