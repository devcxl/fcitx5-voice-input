#include "pipewire_capture.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <thread>

#include <spa/param/audio/format-utils.h>
#include <fcitx-utils/log.h>

#include "capture/audio_buffer_segments.h"

namespace fcitx {
namespace {

// libpipewire 运行时库候选 soname（按优先级）。pipewire 0.3 系列为 .so.0；
// 未来若 soname 变更（如 1.0），旧版本 addon 仍可尝试后续候选。
constexpr const char* kPwLibCandidates[] = {
    "libpipewire-0.3.so.0",
    "libpipewire-1.0.so.0",
};

} // namespace

PipeWireCapture::PipeWireCapture()
    : ringBuffer_(std::make_unique<AudioRingBuffer>(65536)) {}

PipeWireCapture::~PipeWireCapture() {
    Stop();
}

// dlopen 加载 libpipewire 并通过 dlsym 填充函数指针表。
// 返回 true 后所有 pw_* 调用均安全（经 pw_ 间接调用，无未定义符号）；
// 失败则本后端不可用（Start 返回 false，由 pipeline 回退到其他后端），
// 不影响 addon 本体加载。
bool PipeWireCapture::LoadLib() {
    if (pw_.handle) return true;

    for (const char* soname : kPwLibCandidates) {
        void* handle = dlopen(soname, RTLD_NOW | RTLD_GLOBAL);
        if (!handle) continue;

        PwLib lib{};
        lib.handle = handle;
        const char* missingSymbol = nullptr;
#define PW_LOAD(sym)                                                                  \
        do {                                                                          \
            lib.sym = reinterpret_cast<decltype(lib.sym)>(dlsym(handle, #sym));     \
            if (!lib.sym && !missingSymbol) {                                        \
                missingSymbol = #sym;                                                \
            }                                                                         \
        } while (false)
        PW_LOAD(pw_init);
        PW_LOAD(pw_thread_loop_new);
        PW_LOAD(pw_thread_loop_get_loop);
        PW_LOAD(pw_context_new);
        PW_LOAD(pw_context_connect);
        PW_LOAD(pw_context_destroy);
        PW_LOAD(pw_properties_new);
        PW_LOAD(pw_stream_new);
        PW_LOAD(pw_stream_add_listener);
        PW_LOAD(pw_stream_connect);
        PW_LOAD(pw_stream_dequeue_buffer);
        PW_LOAD(pw_stream_queue_buffer);
        PW_LOAD(pw_stream_destroy);
        PW_LOAD(pw_core_disconnect);
        PW_LOAD(pw_thread_loop_destroy);
        PW_LOAD(pw_thread_loop_start);
        PW_LOAD(pw_thread_loop_stop);
        PW_LOAD(pw_stream_state_as_string);
#undef PW_LOAD

        // 符号完整性校验：同名 soname 可能被错误实现/损坏库占用，
        // 任一入口缺失即拒绝，避免后续调用空指针崩溃。
        if (!lib.valid()) {
            FCITX_WARN() << "[voice-input:pw] " << soname
                         << " loaded but required symbol missing: "
                         << (missingSymbol ? missingSymbol : "unknown");
            dlclose(handle);
            continue;
        }

        pw_ = lib;
        FCITX_INFO() << "[voice-input:pw] Runtime-loaded " << soname;
        return true;
    }

    FCITX_WARN() << "[voice-input:pw] Failed to dlopen libpipewire: "
                 << dlerror();
    return false;
}

void PipeWireCapture::UnloadLib() {
    if (pw_.handle) {
        dlclose(pw_.handle);
        pw_ = PwLib{};
    }
}

bool PipeWireCapture::Start() {
    if (running_) return true;

    if (!LoadLib()) return false;

    frameAssembler_.Reset();
    ringBufferDroppedSamples_ = 0;
    drainDiscardedSamples_ = 0;

    pw_.pw_init(nullptr, nullptr);

    loop_ = pw_.pw_thread_loop_new("voice-input-capture", nullptr);
    if (!loop_) {
        FCITX_ERROR() << "[voice-input:pw] Failed to create pw_thread_loop";
        UnloadLib();
        return false;
    }

    context_ = pw_.pw_context_new(pw_.pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_) {
        FCITX_ERROR() << "[voice-input:pw] Failed to create pw_context";
        Cleanup(false);
        UnloadLib();
        return false;
    }

    core_ = pw_.pw_context_connect(context_, nullptr, 0);
    if (!core_) {
        FCITX_ERROR() << "[voice-input:pw] Failed to connect pw_context";
        Cleanup(false);
        UnloadLib();
        return false;
    }

    struct pw_properties* props =
        pw_.pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Communication",
            PW_KEY_NODE_NAME, "voice-input-capture",
            PW_KEY_NODE_DESCRIPTION, "Voice Input Audio Capture",
            nullptr);

    stream_ = pw_.pw_stream_new(core_, "voice-input-capture", props);
    if (!stream_) {
        FCITX_ERROR() << "[voice-input:pw] Failed to create pw_stream";
        Cleanup(false);
        UnloadLib();
        return false;
    }

    static const struct pw_stream_events stream_events = [] {
        struct pw_stream_events events{};
        events.version = PW_VERSION_STREAM_EVENTS;
        events.state_changed = &PipeWireCapture::OnStateChanged;
        events.process = &PipeWireCapture::OnProcess;
        return events;
    }();

    pw_.pw_stream_add_listener(stream_, &streamListener_, &stream_events, this);

    uint8_t buffer[1024];
    spa_audio_info_raw audio_info = {};
    audio_info.format = SPA_AUDIO_FORMAT_F32;
    audio_info.channels = 1;
    audio_info.rate = 16000;

    struct spa_pod_builder podBuilder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&podBuilder, SPA_PARAM_EnumFormat, &audio_info);

    int connectResult = pw_.pw_stream_connect(stream_,
                                          PW_DIRECTION_INPUT,
                                          PW_ID_ANY,
                                          static_cast<pw_stream_flags>(
                                              PW_STREAM_FLAG_AUTOCONNECT |
                                              PW_STREAM_FLAG_MAP_BUFFERS |
                                              PW_STREAM_FLAG_RT_PROCESS),
                                          params, 1);
    if (connectResult < 0) {
        FCITX_ERROR() << "[voice-input:pw] pw_stream_connect failed: " << connectResult;
        Cleanup(false);
        UnloadLib();
        return false;
    }

    if (pw_.pw_thread_loop_start(loop_) < 0) {
        FCITX_ERROR() << "[voice-input:pw] Failed to start pw_thread_loop";
        Cleanup(false);
        UnloadLib();
        return false;
    }

    // Start drain thread: ring buffer → AudioFrame → frameQueue_
    drainRunning_ = true;
    drainThread_ = std::make_unique<std::thread>(&PipeWireCapture::DrainLoop, this);

    running_ = true;
    FCITX_INFO() << "[voice-input:pw] Capture started (with drain thread)";
    return true;
}

void PipeWireCapture::Stop() {
    if (!running_) return;

    // 先停止 PipeWire 回调，再关闭 drain，避免 drain 已退出后继续写入 ring buffer。
    if (loop_) {
        pw_.pw_thread_loop_stop(loop_);
    }

    drainRunning_ = false;
    if (drainThread_ && drainThread_->joinable()) {
        drainThread_->join();
        drainThread_.reset();
    }

    size_t discarded = frameAssembler_.PendingSamples();
    float discardBuffer[kWindowSize];
    while (true) {
        const size_t read = ringBuffer_->Read(discardBuffer, kWindowSize);
        if (read == 0) break;
        discarded += read;
    }
    frameAssembler_.Reset();
    drainDiscardedSamples_.fetch_add(discarded, std::memory_order_relaxed);

    const auto ringDropped = ringBufferDroppedSamples_.exchange(0);
    const auto drainDiscarded = drainDiscardedSamples_.exchange(0);
    if (ringDropped > 0 || drainDiscarded > 0) {
        FCITX_WARN() << "[voice-input:pw] Audio samples dropped: ring="
                     << ringDropped << " drain=" << drainDiscarded;
    }

    Cleanup(false);
    UnloadLib();
    running_ = false;
    FCITX_INFO() << "[voice-input:pw] Capture stopped";
}

void PipeWireCapture::Cleanup(bool stopLoop) {
    if (stopLoop && loop_) {
        pw_.pw_thread_loop_stop(loop_);
    }
    if (stream_) {
        pw_.pw_stream_destroy(stream_);
        stream_ = nullptr;
    }
    if (core_) {
        pw_.pw_core_disconnect(core_);
        core_ = nullptr;
    }
    if (context_) {
        pw_.pw_context_destroy(context_);
        context_ = nullptr;
    }
    if (loop_) {
        pw_.pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }
}

void PipeWireCapture::OnProcess(void* userdata) {
    auto* self = static_cast<PipeWireCapture*>(userdata);
    self->OnProcessImpl();
}

void PipeWireCapture::OnStateChanged(void* userdata, pw_stream_state oldState,
                                     pw_stream_state state, const char* error) {
    auto* self = static_cast<PipeWireCapture*>(userdata);
    FCITX_DEBUG() << "[voice-input:pw] Stream state: "
                 << self->pw_.pw_stream_state_as_string(oldState) << " -> "
                 << self->pw_.pw_stream_state_as_string(state)
                 << (error ? " error=" : "") << (error ? error : "");
}

void PipeWireCapture::OnProcessImpl() {
    pw_buffer* buf = pw_.pw_stream_dequeue_buffer(stream_);
    if (!buf) {
        return;
    }

    struct spa_buffer* spa_buf = buf->buffer;
    if (spa_buf->n_datas == 0) {
        pw_.pw_stream_queue_buffer(stream_, buf);
        return;
    }

    const auto& data = spa_buf->datas[0];
    void* src = data.data;
    auto* chunk = data.chunk;
    if (!chunk) {
        pw_.pw_stream_queue_buffer(stream_, buf);
        return;
    }

    if (!src || data.maxsize == 0 || chunk->size == 0) {
        pw_.pw_stream_queue_buffer(stream_, buf);
        return;
    }

    ForEachCircularFloatSegment(static_cast<const float*>(src), data.maxsize,
                                chunk->offset, chunk->size,
                                [this](const float* pcm, size_t frames) {
                                    PushSamples(pcm, frames);
                                });

    pw_.pw_stream_queue_buffer(stream_, buf);
}

void PipeWireCapture::PushSamples(const float* pcm, size_t frames) {
    if (frames == 0) return;

    const size_t written = ringBuffer_->Write(pcm, frames);
    if (written < frames) {
        ringBufferDroppedSamples_.fetch_add(frames - written,
                                            std::memory_order_relaxed);
    }
}

void PipeWireCapture::DrainLoop() {
    constexpr size_t kDrainChunk = 512;
    float floatBuf[kDrainChunk];

    while (drainRunning_) {
        size_t read = ringBuffer_->Read(floatBuf, kDrainChunk);
        if (read == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        frameAssembler_.Append(floatBuf, read,
            [this](const std::array<float, kWindowSize>& samples) {
                if (!frameQueue_) return;

                AudioFrame frame;
                frame.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count();

                static constexpr float kFloatToInt16 = 32767.0f;
                for (size_t i = 0; i < samples.size(); ++i) {
                    float s = std::clamp(samples[i], -1.0f, 1.0f);
                    frame.pcm[i] = static_cast<int16_t>(s * kFloatToInt16);
                }

                frameQueue_->Push(frame);
            });
    }

}

} // namespace fcitx
