// 假 libpulse-simple 实现，仅在测试中通过 LD_LIBRARY_PATH 加载。
// 见 tests/support/fake_pulse.h 的接口说明。

#include "fake_pulse.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <pulse/error.h>
#include <pulse/simple.h>

namespace {

constexpr int kMagic = 0x5A5A1234;

struct State {
    std::mutex mutex;

    unsigned newCount = 0;
    unsigned freeCount = 0;
    unsigned readCount = 0;

    bool devWasNull = true;
    std::string dev;
    std::string appName;
    std::string streamName;
    int direction = -1;
    bool haveSampleSpec = false;
    int sampleFormat = -1;
    uint32_t sampleRate = 0;
    uint8_t channels = 0;

    bool failNew = false;
    bool failRead = false;
    int16_t readSampleValue = 0;
    unsigned readDelayMs = 2;
};

State& G() {
    static State state;
    return state;
}

template <typename Fn>
auto Read(Fn&& fn) {
    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    return fn(g);
}

std::string& NameBuffer() {
    static thread_local std::string buffer;
    return buffer;
}

} // namespace

// 不透明句柄：真实 SDK 只做前向声明，这里给出最小定义以便分配/释放。
struct pa_simple {
    int magic = kMagic;
};

extern "C" {

pa_simple* pa_simple_new(const char* server,
                         const char* name,
                         pa_stream_direction_t dir,
                         const char* dev,
                         const char* stream_name,
                         const pa_sample_spec* ss,
                         const pa_channel_map* map,
                         const pa_buffer_attr* attr,
                         int* error) {
    (void)map;
    (void)attr;

    return Read([&](State& g) -> pa_simple* {
        ++g.newCount;
        (void)server;
        g.appName = name ? name : "";
        g.direction = static_cast<int>(dir);
        g.devWasNull = (dev == nullptr);
        g.dev = dev ? dev : "";
        g.streamName = stream_name ? stream_name : "";
        g.haveSampleSpec = (ss != nullptr);
        if (ss) {
            g.sampleFormat = static_cast<int>(ss->format);
            g.sampleRate = ss->rate;
            g.channels = ss->channels;
        }
        if (g.failNew) {
            if (error) *error = PA_ERR_INVALID;
            return nullptr;
        }
        return new pa_simple{};
    });
}

void pa_simple_free(pa_simple* s) {
    if (!s) return;
    Read([&](State& g) { ++g.freeCount; });
    delete s;
}

int pa_simple_read(pa_simple* s, void* data, size_t bytes, int* error) {
    (void)s;
    struct ReadSnapshot { unsigned delayMs; int16_t sample; bool fail; };
    const auto snapshot = Read([](State& g) {
        ++g.readCount;
        return ReadSnapshot{g.readDelayMs, g.readSampleValue, g.failRead};
    });
    if (snapshot.delayMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(snapshot.delayMs));
    }
    if (snapshot.fail) {
        if (error) *error = PA_ERR_BADSTATE;
        return -1;
    }
    const int16_t sample = snapshot.sample;
    auto* out = static_cast<uint8_t*>(data);
    size_t filled = 0;
    while (data && filled + sizeof(int16_t) <= bytes) {
        std::memcpy(out + filled, &sample, sizeof(int16_t));
        filled += sizeof(int16_t);
    }
    for (; data && filled < bytes; ++filled) out[filled] = 0;
    return 0;
}

const char* pa_strerror(int error) {
    (void)error;
    return "fake pulse error";
}

void fakepulse_reset(void) {
    // State 含 std::mutex，不可整体赋值，逐字段恢复默认值。
    Read([](State& g) {
        g.newCount = 0;
        g.freeCount = 0;
        g.readCount = 0;
        g.devWasNull = true;
        g.dev.clear();
        g.appName.clear();
        g.streamName.clear();
        g.direction = -1;
        g.haveSampleSpec = false;
        g.sampleFormat = -1;
        g.sampleRate = 0;
        g.channels = 0;
        g.failNew = false;
        g.failRead = false;
        g.readSampleValue = 0;
        g.readDelayMs = 2;
    });
}

unsigned fakepulse_new_count(void) {
    return Read([](State& g) { return g.newCount; });
}

unsigned fakepulse_free_count(void) {
    return Read([](State& g) { return g.freeCount; });
}

unsigned fakepulse_read_count(void) {
    return Read([](State& g) { return g.readCount; });
}

int fakepulse_dev_is_null(void) {
    return Read([](State& g) { return g.devWasNull ? 1 : 0; });
}

const char* fakepulse_last_dev(void) {
    return Read([](State& g) -> const char* {
        NameBuffer() = g.dev;
        return NameBuffer().c_str();
    });
}

const char* fakepulse_app_name(void) {
    return Read([](State& g) -> const char* {
        NameBuffer() = g.appName;
        return NameBuffer().c_str();
    });
}

const char* fakepulse_stream_name(void) {
    return Read([](State& g) -> const char* {
        NameBuffer() = g.streamName;
        return NameBuffer().c_str();
    });
}

int fakepulse_stream_direction(void) {
    return Read([](State& g) { return g.direction; });
}

int fakepulse_have_sample_spec(void) {
    return Read([](State& g) { return g.haveSampleSpec ? 1 : 0; });
}

int fakepulse_sample_format(void) {
    return Read([](State& g) { return g.sampleFormat; });
}

unsigned fakepulse_sample_rate(void) {
    return Read([](State& g) { return g.sampleRate; });
}

unsigned fakepulse_channels(void) {
    return Read([](State& g) { return g.channels; });
}

void fakepulse_set_fail_new(int fail) {
    Read([&](State& g) { g.failNew = (fail != 0); });
}

void fakepulse_set_fail_read(int fail) {
    Read([&](State& g) { g.failRead = (fail != 0); });
}

void fakepulse_set_read_sample_value(int value) {
    Read([&](State& g) { g.readSampleValue = static_cast<int16_t>(value); });
}

void fakepulse_set_read_delay_ms(unsigned ms) {
    Read([&](State& g) { g.readDelayMs = ms; });
}

} // extern "C"
