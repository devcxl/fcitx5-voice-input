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
    // 运行期 dlopen + dlsym 函数指针表。所有 pa_* 调用经由此表间接调用，
    // addon 无任何 pa_* 未定义符号——即使 Arch 构建的 DF_1_NOW (-z,now)
    // 强制立即绑定，addon 也能正常加载（见 PR #20）。
    struct PulseLib {
        void* handle = nullptr;
        pa_simple* (*pa_simple_new)(const char*, const char*, pa_stream_direction,
                                    const char*, const char*, const pa_sample_spec*,
                                    const pa_channel_map*, const pa_buffer_attr*, int*) = nullptr;
        void (*pa_simple_free)(pa_simple*) = nullptr;
        int (*pa_simple_read)(pa_simple*, void*, size_t, int*) = nullptr;
        const char* (*pa_strerror)(int) = nullptr;
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
