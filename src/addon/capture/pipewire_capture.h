#pragma once

#include <memory>
#include <functional>
#include <pipewire/pipewire.h>
#include "capture/audio_capture.h"

namespace fcitx {

/**
 * PipeWire audio capture wrapper.
 *
 * Manages a pw_thread_loop for audio capture from the default input device.
 * The on_process callback writes PCM frames into a lock-free ring buffer
 * and returns immediately — no VAD, no allocation, no blocking.
 *
 * Thread safety:
 * - Start/Stop must be called from the same thread (main thread).
 * - OnAudioData callback runs on the PipeWire thread (locked).
 * - Ring buffer is SPSC: producer = PipeWire callback, consumer = VAD thread.
 */
class PipeWireCapture : public AudioCapture {
public:
    using AudioDataCallback = std::function<void(const float* pcm, size_t frames)>;

    PipeWireCapture();
    ~PipeWireCapture();

    PipeWireCapture(const PipeWireCapture&) = delete;
    PipeWireCapture& operator=(const PipeWireCapture&) = delete;

    bool Start() override;
    void Stop() override;
    bool IsRunning() const override { return running_; }
    const char* Name() const override { return "pipewire"; }

    // Register a callback for raw PCM data (optional, for testing).
    // If set, called from the PipeWire thread — keep it minimal!
    void SetRawCallback(AudioDataCallback cb) { rawCallback_ = std::move(cb); }

    const AudioRingBuffer* RingBuffer() const override { return ringBuffer_.get(); }
    AudioRingBuffer* RingBuffer() override { return ringBuffer_.get(); }

private:
    static void OnProcess(void* userdata);
    static void OnStateChanged(void* userdata, pw_stream_state oldState,
                               pw_stream_state state, const char* error);
    void OnProcessImpl();
    void Cleanup(bool stopLoop);

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;

    std::unique_ptr<AudioRingBuffer> ringBuffer_;
    std::atomic<bool> running_{false};
    AudioDataCallback rawCallback_;

    spa_hook streamListener_{};
    spa_hook coreListener_{};
};

} // namespace fcitx
