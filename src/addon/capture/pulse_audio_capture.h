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

private:
    // 运行期 dlopen 句柄（RTLD_GLOBAL 加载 libpulse-simple，避免链接期
    // DT_NEEDED 硬依赖——库升级/soname 变更时 addon 仍可加载）
    void* pulseLib_ = nullptr;
    bool LoadLib();
    void UnloadLib();
    void CaptureLoop();

    pa_simple* stream_ = nullptr;
    std::unique_ptr<std::thread> captureThread_;
    std::atomic<bool> running_{false};
};

} // namespace fcitx
