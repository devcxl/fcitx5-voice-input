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
    bool LoadLib();
    void UnloadLib();
    void Cleanup(bool stopLoop);
    void DrainLoop();

    // 运行期 dlopen + dlsym 函数指针表。所有 pw_* 调用经由此表间接调用，
    // addon 无任何 pw_* 未定义符号——即使 Arch 构建的 DF_1_NOW (-z,now)
    // 强制立即绑定，addon 也能正常加载（见 PR #20）。
    struct PwLib {
        void* handle = nullptr;
        void (*pw_init)(int*, char***) = nullptr;
        pw_thread_loop* (*pw_thread_loop_new)(const char*, const struct spa_dict*) = nullptr;
        pw_loop* (*pw_thread_loop_get_loop)(pw_thread_loop*) = nullptr;
        pw_context* (*pw_context_new)(pw_loop*, pw_properties*, size_t) = nullptr;
        pw_core* (*pw_context_connect)(pw_context*, pw_properties*, size_t) = nullptr;
        void (*pw_context_destroy)(pw_context*) = nullptr;
        pw_properties* (*pw_properties_new)(const char*, ...) = nullptr;
        pw_stream* (*pw_stream_new)(pw_core*, const char*, pw_properties*) = nullptr;
        int (*pw_stream_add_listener)(pw_stream*, struct spa_hook*,
                                      const pw_stream_events*, void*) = nullptr;
        int (*pw_stream_connect)(pw_stream*, pw_direction, uint32_t, pw_stream_flags,
                                 const spa_pod**, uint32_t) = nullptr;
        pw_buffer* (*pw_stream_dequeue_buffer)(pw_stream*) = nullptr;
        int (*pw_stream_queue_buffer)(pw_stream*, pw_buffer*) = nullptr;
        void (*pw_stream_destroy)(pw_stream*) = nullptr;
        void (*pw_core_disconnect)(pw_core*) = nullptr;
        void (*pw_thread_loop_destroy)(pw_thread_loop*) = nullptr;
        int (*pw_thread_loop_start)(pw_thread_loop*) = nullptr;
        void (*pw_thread_loop_stop)(pw_thread_loop*) = nullptr;
        const char* (*pw_stream_state_as_string)(pw_stream_state) = nullptr;
        bool valid() const {
            return handle && pw_init && pw_thread_loop_new && pw_thread_loop_get_loop &&
                   pw_context_new && pw_context_connect && pw_context_destroy &&
                   pw_properties_new && pw_stream_new && pw_stream_add_listener &&
                   pw_stream_connect && pw_stream_dequeue_buffer && pw_stream_queue_buffer &&
                   pw_stream_destroy && pw_core_disconnect && pw_thread_loop_destroy &&
                   pw_thread_loop_start && pw_thread_loop_stop && pw_stream_state_as_string;
        }
    };

    PwLib pw_;
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
