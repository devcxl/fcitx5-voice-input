#include "pipewire_capture.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include <spa/param/audio/format-utils.h>
#include <fcitx-utils/log.h>

namespace fcitx {

PipeWireCapture::PipeWireCapture()
    : ringBuffer_(std::make_unique<AudioRingBuffer>(65536)) {}

PipeWireCapture::~PipeWireCapture() {
    Stop();
}

bool PipeWireCapture::Start() {
    if (running_) return true;

    pw_init(nullptr, nullptr);

    loop_ = pw_thread_loop_new("voice-input-capture", nullptr);
    if (!loop_) {
        FCITX_ERROR() << "[voice-input:pw] Failed to create pw_thread_loop";
        return false;
    }

    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_) {
        FCITX_ERROR() << "[voice-input:pw] Failed to create pw_context";
        Cleanup(false);
        return false;
    }

    core_ = pw_context_connect(context_, nullptr, 0);
    if (!core_) {
        FCITX_ERROR() << "[voice-input:pw] Failed to connect pw_context";
        Cleanup(false);
        return false;
    }

    struct pw_properties* props =
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Communication",
            PW_KEY_NODE_NAME, "voice-input-capture",
            PW_KEY_NODE_DESCRIPTION, "Voice Input Audio Capture",
            nullptr);

    stream_ = pw_stream_new(core_, "voice-input-capture", props);
    if (!stream_) {
        FCITX_ERROR() << "[voice-input:pw] Failed to create pw_stream";
        Cleanup(false);
        return false;
    }

    static const struct pw_stream_events stream_events = [] {
        struct pw_stream_events events{};
        events.version = PW_VERSION_STREAM_EVENTS;
        events.state_changed = &PipeWireCapture::OnStateChanged;
        events.process = &PipeWireCapture::OnProcess;
        return events;
    }();

    pw_stream_add_listener(stream_, &streamListener_, &stream_events, this);

    uint8_t buffer[1024];
    spa_audio_info_raw audio_info = {};
    audio_info.format = SPA_AUDIO_FORMAT_F32;
    audio_info.channels = 1;
    audio_info.rate = 16000;

    struct spa_pod_builder podBuilder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&podBuilder, SPA_PARAM_EnumFormat, &audio_info);

    int connectResult = pw_stream_connect(stream_,
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
        return false;
    }

    if (pw_thread_loop_start(loop_) < 0) {
        FCITX_ERROR() << "[voice-input:pw] Failed to start pw_thread_loop";
        Cleanup(false);
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

    // Stop drain thread first
    drainRunning_ = false;
    if (drainThread_ && drainThread_->joinable()) {
        drainThread_->join();
        drainThread_.reset();
    }

    Cleanup(true);
    running_ = false;
    FCITX_INFO() << "[voice-input:pw] Capture stopped";
}

void PipeWireCapture::Cleanup(bool stopLoop) {
    if (stopLoop && loop_) {
        pw_thread_loop_stop(loop_);
    }
    if (stream_) {
        pw_stream_destroy(stream_);
        stream_ = nullptr;
    }
    if (core_) {
        pw_core_disconnect(core_);
        core_ = nullptr;
    }
    if (context_) {
        pw_context_destroy(context_);
        context_ = nullptr;
    }
    if (loop_) {
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }
}

void PipeWireCapture::OnProcess(void* userdata) {
    auto* self = static_cast<PipeWireCapture*>(userdata);
    self->OnProcessImpl();
}

void PipeWireCapture::OnStateChanged(void*, pw_stream_state oldState,
                                     pw_stream_state state, const char* error) {
    FCITX_INFO() << "[voice-input:pw] Stream state: "
                 << pw_stream_state_as_string(oldState) << " -> "
                 << pw_stream_state_as_string(state)
                 << (error ? " error=" : "") << (error ? error : "");
}

void PipeWireCapture::OnProcessImpl() {
    pw_buffer* buf = pw_stream_dequeue_buffer(stream_);
    if (!buf) {
        FCITX_DEBUG() << "[voice-input:pw] dequeue_buffer returned null";
        return;
    }

    struct spa_buffer* spa_buf = buf->buffer;
    if (spa_buf->n_datas == 0) {
        pw_stream_queue_buffer(stream_, buf);
        return;
    }

    void* src = spa_buf->datas[0].data;
    auto* chunk = spa_buf->datas[0].chunk;
    if (!chunk) {
        pw_stream_queue_buffer(stream_, buf);
        return;
    }
    uint32_t size = chunk->size;

    if (!src || size == 0) {
        pw_stream_queue_buffer(stream_, buf);
        return;
    }

    size_t frames = size / sizeof(float);
    const float* pcm = static_cast<const float*>(src);
    ringBuffer_->Write(pcm, frames);

    if (rawCallback_) {
        rawCallback_(pcm, frames);
    }

    pw_stream_queue_buffer(stream_, buf);
}

void PipeWireCapture::DrainLoop() {
    constexpr size_t kDrainChunk = 512;
    float floatBuf[kDrainChunk];

    while (drainRunning_) {
        size_t read = ringBuffer_->Read(floatBuf, kDrainChunk);
        if (read < kDrainChunk) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        if (!frameQueue_) continue;

        AudioFrame frame;
        frame.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();

        static constexpr float kFloatToInt16 = 32767.0f;
        for (size_t i = 0; i < kDrainChunk; ++i) {
            float s = std::clamp(floatBuf[i], -1.0f, 1.0f);
            frame.pcm[i] = static_cast<int16_t>(s * kFloatToInt16);
        }

        frameQueue_->Push(frame);
    }
}

} // namespace fcitx
