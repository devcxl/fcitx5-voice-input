#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#include <pipewire/pipewire.h>

#include "capture/audio_capture.h"
#include "utils/audio_buffer.h"

namespace fcitx {

class PipeWireCapture : public AudioCapture {
public:
    using AudioDataCallback = std::function<void(const float* pcm, size_t frames)>;

    PipeWireCapture();
    ~PipeWireCapture() override;

    PipeWireCapture(const PipeWireCapture&) = delete;
    PipeWireCapture& operator=(const PipeWireCapture&) = delete;

    bool Start() override;
    void Stop() override;
    bool IsRunning() const override { return running_; }
    const char* Name() const override { return "pipewire"; }

    void SetRawCallback(AudioDataCallback cb) { rawCallback_ = std::move(cb); }

private:
    static void OnProcess(void* userdata);
    static void OnStateChanged(void* userdata, pw_stream_state oldState,
                               pw_stream_state state, const char* error);
    void OnProcessImpl();
    void Cleanup(bool stopLoop);
    void DrainLoop();

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;

    std::unique_ptr<AudioRingBuffer> ringBuffer_;
    std::unique_ptr<std::thread> drainThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> drainRunning_{false};
    AudioDataCallback rawCallback_;

    spa_hook streamListener_{};
    spa_hook coreListener_{};
};

} // namespace fcitx
