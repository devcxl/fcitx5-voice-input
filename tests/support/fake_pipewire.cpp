// 假 libpipewire-0.3 实现，仅在测试中通过 LD_LIBRARY_PATH 加载。
// 见 tests/support/fake_pipewire.h 的接口说明。
//
// 编译期直接包含上游 <pipewire/pipewire.h>，因此导出的 18 个 pw_* 入口与 SDK
// 声明逐字一致（含 pw_properties_new / pw_init 的可变参数与 SPA_SENTINEL），
// 无需手写、猜测函数指针签名。

#include "fake_pipewire.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include <pipewire/pipewire.h>

namespace {

constexpr unsigned kDefaultSamplesPerBuffer = 512;
constexpr unsigned kDefaultCallbackIntervalMs = 2;
constexpr auto kHoldTimeout = std::chrono::seconds(2);

// 单一全局状态：测试进程内只会有一个 capture / 一条 stream 处于活动状态。
struct State {
    std::mutex mutex;
    std::condition_variable cv;

    unsigned streamNew = 0;
    unsigned streamDestroy = 0;
    unsigned streamConnect = 0;
    unsigned streamQueue = 0;
    unsigned coreDisconnect = 0;
    unsigned contextDestroy = 0;
    unsigned loopStart = 0;
    unsigned loopStop = 0;
    unsigned loopDestroy = 0;
    unsigned dequeue = 0;

    float sampleValue = 0.0f;
    unsigned samplesPerBuffer = kDefaultSamplesPerBuffer;
    unsigned callbackIntervalMs = kDefaultCallbackIntervalMs;

    bool holdArmed = false;
    bool dequeueHeld = false;
    bool releaseHeld = false;
    float heldValue = 0.0f;

    bool haveStream = false;
    bool connected = false;
    pw_stream_events events{};
    void* userdata = nullptr;
    void* stream = nullptr;

    bool running = false;
    std::thread thread;
};

State& G() {
    static State state;
    return state;
}

// 不透明句柄：真实 SDK 只做前向声明，这里给出最小定义以便分配/释放。
struct FakeLoop {
    int magic = 0x10;
};
struct FakeContext {
    int magic = 0x20;
};
struct FakeCore {
    int magic = 0x30;
};

// 一个可复用的 capture buffer：vector 持有 float 数据，spa_buffer/spa_data/
// spa_chunk/pw_buffer 与之绑定（vector 在设置指针前一次性 assign，不再扩容）。
struct FakeStream {
    std::vector<float> samples;
    spa_chunk chunk{};
    spa_data data{};
    spa_buffer buffer{};
    pw_buffer pwbuf{};
};

void InitBuffer(FakeStream* stream, unsigned sampleCount) {
    const unsigned count = sampleCount ? sampleCount : kDefaultSamplesPerBuffer;
    stream->samples.assign(count, 0.0f);

    const uint32_t bytes = static_cast<uint32_t>(count * sizeof(float));
    stream->chunk.offset = 0;
    stream->chunk.size = bytes;
    stream->chunk.stride = static_cast<int32_t>(sizeof(float));
    stream->chunk.flags = 0;

    stream->data.type = SPA_DATA_MemPtr;
    stream->data.flags = SPA_DATA_FLAG_READABLE;
    stream->data.fd = -1;
    stream->data.mapoffset = 0;
    stream->data.maxsize = bytes;
    stream->data.data = stream->samples.data();
    stream->data.chunk = &stream->chunk;

    stream->buffer.n_metas = 0;
    stream->buffer.metas = nullptr;
    stream->buffer.n_datas = 1;
    stream->buffer.datas = &stream->data;

    stream->pwbuf.buffer = &stream->buffer;
    stream->pwbuf.user_data = nullptr;
    stream->pwbuf.size = 0;
    stream->pwbuf.requested = 0;
}

template <typename Fn>
auto Read(Fn&& fn) {
    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    return fn(g);
}

} // namespace

extern "C" {

// ── pw_* SDK 精确签名 ──────────────────────────────────────────────────

void pw_init(int* argc, char*** argv) {
    (void)argc;
    (void)argv;
}

void pw_deinit(void) {}

struct pw_thread_loop* pw_thread_loop_new(const char* name, const struct spa_dict* props) {
    (void)name;
    (void)props;
    return reinterpret_cast<pw_thread_loop*>(new FakeLoop{});
}

struct pw_loop* pw_thread_loop_get_loop(struct pw_thread_loop* loop) {
    return reinterpret_cast<pw_loop*>(loop);
}

struct pw_context* pw_context_new(struct pw_loop* main_loop, struct pw_properties* props,
                                  size_t user_data_size) {
    (void)main_loop;
    (void)user_data_size;
    if (props) std::free(props);
    return reinterpret_cast<pw_context*>(new FakeContext{});
}

struct pw_core* pw_context_connect(struct pw_context* context, struct pw_properties* props,
                                   size_t user_data_size) {
    (void)context;
    (void)user_data_size;
    if (props) std::free(props);
    return reinterpret_cast<pw_core*>(new FakeCore{});
}

void pw_context_destroy(struct pw_context* context) {
    Read([](State& g) { ++g.contextDestroy; });
    delete reinterpret_cast<FakeContext*>(context);
}

struct pw_properties* pw_properties_new(const char* key, ...) {
    (void)key;
    // 所有权随即交给 pw_stream_new/pw_context_connect，用 free 释放即可。
    return static_cast<struct pw_properties*>(std::calloc(1, sizeof(struct pw_properties)));
}

struct pw_stream* pw_stream_new(struct pw_core* core, const char* name,
                                struct pw_properties* props) {
    (void)core;
    (void)name;
    if (props) std::free(props);

    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    auto* stream = new FakeStream{};
    InitBuffer(stream, g.samplesPerBuffer);
    g.stream = stream;
    g.haveStream = false;
    g.connected = false;
    ++g.streamNew;
    return reinterpret_cast<pw_stream*>(stream);
}

void pw_stream_add_listener(struct pw_stream* stream, struct spa_hook* listener,
                            const struct pw_stream_events* events, void* data) {
    (void)listener;
    Read([&](State& g) {
        g.events = *events;
        g.userdata = data;
        g.haveStream = true;
        g.stream = reinterpret_cast<FakeStream*>(stream);
    });
}

int pw_stream_connect(struct pw_stream* stream, enum pw_direction direction,
                      uint32_t target_id, enum pw_stream_flags flags,
                      const struct spa_pod** params, uint32_t n_params) {
    (void)direction;
    (void)target_id;
    (void)flags;
    (void)params;
    (void)n_params;
    Read([&](State& g) {
        ++g.streamConnect;
        g.stream = reinterpret_cast<FakeStream*>(stream);
        g.connected = true;
    });
    return 0;
}

struct pw_buffer* pw_stream_dequeue_buffer(struct pw_stream* stream) {
    auto* s = reinterpret_cast<FakeStream*>(stream);
    State& g = G();
    std::unique_lock<std::mutex> lock(g.mutex);
    ++g.dequeue;

    float value = g.sampleValue;
    if (g.holdArmed) {
        // 阻塞点：快照“旧”样本值并等待释放（或 2s 超时），模拟一个已取 epoch、
        // 仍在写的 RT 回调跨 pause/resume。
        g.holdArmed = false;
        g.heldValue = value;
        g.dequeueHeld = true;
        g.cv.notify_all();
        g.cv.wait_for(lock, kHoldTimeout, [&] { return g.releaseHeld; });
        g.dequeueHeld = false;
        g.releaseHeld = false;
        value = g.heldValue;
    }
    lock.unlock();

    for (float& sample : s->samples) sample = value;
    return &s->pwbuf;
}

int pw_stream_queue_buffer(struct pw_stream* stream, struct pw_buffer* buffer) {
    (void)stream;
    (void)buffer;
    Read([](State& g) { ++g.streamQueue; });
    return 0;
}

void pw_stream_destroy(struct pw_stream* stream) {
    Read([&](State& g) {
        ++g.streamDestroy;
        if (g.stream == reinterpret_cast<FakeStream*>(stream)) {
            g.stream = nullptr;
            g.haveStream = false;
            g.connected = false;
            g.events = pw_stream_events{};
            g.userdata = nullptr;
        }
    });
    delete reinterpret_cast<FakeStream*>(stream);
}

int pw_core_disconnect(struct pw_core* core) {
    Read([](State& g) { ++g.coreDisconnect; });
    delete reinterpret_cast<FakeCore*>(core);
    return 0;
}

void pw_thread_loop_destroy(struct pw_thread_loop* loop) {
    Read([](State& g) { ++g.loopDestroy; });
    delete reinterpret_cast<FakeLoop*>(loop);
}

int pw_thread_loop_start(struct pw_thread_loop* loop) {
    (void)loop;
    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    ++g.loopStart;
    if (g.running) return 0;
    g.running = true;
    g.thread = std::thread([] {
        State& state = G();
        for (;;) {
            pw_stream_events events{};
            void* userdata = nullptr;
            bool deliver = false;
            unsigned interval = kDefaultCallbackIntervalMs;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                if (!state.running) break;
                interval = state.callbackIntervalMs;
                if (state.connected && state.haveStream) {
                    events = state.events;
                    userdata = state.userdata;
                    deliver = true;
                }
            }
            if (deliver && events.process) events.process(userdata);
            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        }
    });
    return 0;
}

void pw_thread_loop_stop(struct pw_thread_loop* loop) {
    (void)loop;
    State& g = G();
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        ++g.loopStop;
        if (!g.running) return;
        g.running = false;
        // 释放可能被 held-next-dequeue 阻塞的回调，保证 join 有界。
        g.releaseHeld = true;
        g.cv.notify_all();
        thread = std::move(g.thread);
    }
    if (thread.joinable()) thread.join();
}

const char* pw_stream_state_as_string(enum pw_stream_state state) {
    switch (state) {
    case PW_STREAM_STATE_ERROR:
        return "error";
    case PW_STREAM_STATE_UNCONNECTED:
        return "unconnected";
    case PW_STREAM_STATE_CONNECTING:
        return "connecting";
    case PW_STREAM_STATE_PAUSED:
        return "paused";
    case PW_STREAM_STATE_STREAMING:
        return "streaming";
    }
    return "unknown";
}

// ── fakepw_* 控制接口 ─────────────────────────────────────────────────

void fakepw_reset(void) {
    Read([](State& g) {
        g.streamNew = 0;
        g.streamDestroy = 0;
        g.streamConnect = 0;
        g.streamQueue = 0;
        g.coreDisconnect = 0;
        g.contextDestroy = 0;
        g.loopStart = 0;
        g.loopStop = 0;
        g.loopDestroy = 0;
        g.dequeue = 0;
        g.sampleValue = 0.0f;
        g.samplesPerBuffer = kDefaultSamplesPerBuffer;
        g.callbackIntervalMs = kDefaultCallbackIntervalMs;
        g.holdArmed = false;
        g.dequeueHeld = false;
        g.releaseHeld = false;
        g.heldValue = 0.0f;
        g.haveStream = false;
        g.events = pw_stream_events{};
        g.userdata = nullptr;
    });
}

unsigned fakepw_stream_new_count(void) {
    return Read([](State& g) { return g.streamNew; });
}
unsigned fakepw_stream_destroy_count(void) {
    return Read([](State& g) { return g.streamDestroy; });
}
unsigned fakepw_stream_connect_count(void) {
    return Read([](State& g) { return g.streamConnect; });
}
unsigned fakepw_stream_queue_count(void) {
    return Read([](State& g) { return g.streamQueue; });
}
unsigned fakepw_core_disconnect_count(void) {
    return Read([](State& g) { return g.coreDisconnect; });
}
unsigned fakepw_context_destroy_count(void) {
    return Read([](State& g) { return g.contextDestroy; });
}
unsigned fakepw_thread_loop_start_count(void) {
    return Read([](State& g) { return g.loopStart; });
}
unsigned fakepw_thread_loop_stop_count(void) {
    return Read([](State& g) { return g.loopStop; });
}
unsigned fakepw_thread_loop_destroy_count(void) {
    return Read([](State& g) { return g.loopDestroy; });
}
unsigned fakepw_dequeue_count(void) {
    return Read([](State& g) { return g.dequeue; });
}

void fakepw_set_sample_value(float value) {
    Read([&](State& g) { g.sampleValue = value; });
}
void fakepw_set_samples_per_buffer(unsigned count) {
    Read([&](State& g) { g.samplesPerBuffer = count; });
}
void fakepw_set_callback_interval_ms(unsigned ms) {
    Read([&](State& g) { g.callbackIntervalMs = ms; });
}

void fakepw_hold_next_dequeue(void) {
    Read([](State& g) { g.holdArmed = true; });
}
int fakepw_dequeue_held(void) {
    return Read([](State& g) { return g.dequeueHeld ? 1 : 0; });
}
void fakepw_release_held_dequeue(void) {
    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    g.releaseHeld = true;
    g.cv.notify_all();
}
float fakepw_held_sample_value(void) {
    return Read([](State& g) { return g.heldValue; });
}

void fakepw_trigger_stream_error(const char* message) {
    pw_stream_events events{};
    void* userdata = nullptr;
    bool hasStream = false;
    Read([&](State& g) {
        if (g.haveStream) {
            events = g.events;
            userdata = g.userdata;
            hasStream = true;
        }
    });
    if (hasStream && events.state_changed) {
        events.state_changed(userdata, PW_STREAM_STATE_STREAMING, PW_STREAM_STATE_ERROR,
                            message);
    }
}

} // extern "C"
