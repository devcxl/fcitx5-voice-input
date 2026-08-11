#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include <pulse/error.h>
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
    // 运行期 dlopen + dlsym 函数指针表。所有 pa_* 调用经由此表间接调用，
    // addon 无任何 pa_* 未定义符号——即使 Arch 构建的 DF_1_NOW (-z,now)
    // 强制立即绑定，addon 也能正常加载（见 PR #20）。
    struct PulseLib {
        void* handle = nullptr;
        // 从编译期头文件推导精确 ABI 签名，避免手写函数指针与上游 API 漂移。
        decltype(&::pa_simple_new) pa_simple_new = nullptr;
        decltype(&::pa_simple_free) pa_simple_free = nullptr;
        decltype(&::pa_simple_read) pa_simple_read = nullptr;
        decltype(&::pa_strerror) pa_strerror = nullptr;
        bool valid() const { return handle && pa_simple_new && pa_simple_free && pa_simple_read && pa_strerror; }
    };

    PulseLib pulse_;
    bool LoadLib();
    void UnloadLib();
    void CaptureLoop();

    pa_simple* stream_ = nullptr;
    std::unique_ptr<std::thread> captureThread_;
    std::atomic<bool> running_{false};
};

} // namespace fcitx
