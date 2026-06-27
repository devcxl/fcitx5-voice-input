#pragma once

#include <atomic>
#include <memory>
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

    const AudioRingBuffer* RingBuffer() const override { return ringBuffer_.get(); }
    AudioRingBuffer* RingBuffer() override { return ringBuffer_.get(); }

private:
    void CaptureLoop();

    std::unique_ptr<AudioRingBuffer> ringBuffer_;
    pa_simple* stream_ = nullptr;
    std::unique_ptr<std::thread> captureThread_;
    std::atomic<bool> running_{false};
};

} // namespace fcitx
