#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include <pipewire/pipewire.h>

#include "capture/audio_capture.h"
#include "capture/audio_frame_assembler.h"
#include "utils/audio_buffer.h"

namespace fcitx {

class PipeWireCapture : public AudioCapture {
public:
    PipeWireCapture();
    ~PipeWireCapture() override;

    PipeWireCapture(const PipeWireCapture&) = delete;
    PipeWireCapture& operator=(const PipeWireCapture&) = delete;

    bool Start() override;
    void Stop() override;
    bool IsRunning() const override { return running_; }
    const char* Name() const override { return "pipewire"; }

private:
    static void OnProcess(void* userdata);
    static void OnStateChanged(void* userdata, pw_stream_state oldState,
                               pw_stream_state state, const char* error);
    void OnProcessImpl();
    bool LoadLib();
    void UnloadLib();
    void Cleanup(bool stopLoop);
    void DrainLoop();
    void PushSamples(const float* pcm, size_t frames);

    // 运行期 dlopen + dlsym 函数指针表。所有 pw_* 调用经由此表间接调用，
    // addon 无任何 pw_* 未定义符号——即使 Arch 构建的 DF_1_NOW (-z,now)
    // 强制立即绑定，addon 也能正常加载（见 PR #20）。
    struct PwLib {
        void* handle = nullptr;
        // 从编译期头文件推导精确 ABI 签名，避免手写函数指针与上游 API 漂移。
        decltype(&::pw_init) pw_init = nullptr;
        decltype(&::pw_thread_loop_new) pw_thread_loop_new = nullptr;
        decltype(&::pw_thread_loop_get_loop) pw_thread_loop_get_loop = nullptr;
        decltype(&::pw_context_new) pw_context_new = nullptr;
        decltype(&::pw_context_connect) pw_context_connect = nullptr;
        decltype(&::pw_context_destroy) pw_context_destroy = nullptr;
        decltype(&::pw_properties_new) pw_properties_new = nullptr;
        decltype(&::pw_stream_new) pw_stream_new = nullptr;
        decltype(&::pw_stream_add_listener) pw_stream_add_listener = nullptr;
        decltype(&::pw_stream_connect) pw_stream_connect = nullptr;
        decltype(&::pw_stream_dequeue_buffer) pw_stream_dequeue_buffer = nullptr;
        decltype(&::pw_stream_queue_buffer) pw_stream_queue_buffer = nullptr;
        decltype(&::pw_stream_destroy) pw_stream_destroy = nullptr;
        decltype(&::pw_core_disconnect) pw_core_disconnect = nullptr;
        decltype(&::pw_thread_loop_destroy) pw_thread_loop_destroy = nullptr;
        decltype(&::pw_thread_loop_start) pw_thread_loop_start = nullptr;
        decltype(&::pw_thread_loop_stop) pw_thread_loop_stop = nullptr;
        decltype(&::pw_stream_state_as_string) pw_stream_state_as_string = nullptr;
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
    AudioFrameAssembler frameAssembler_;
    std::unique_ptr<std::thread> drainThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> drainRunning_{false};
    std::atomic<uint64_t> ringBufferDroppedSamples_{0};
    std::atomic<uint64_t> drainDiscardedSamples_{0};

    spa_hook streamListener_{};
    spa_hook coreListener_{};
};

} // namespace fcitx
