// PipeWire 直连采集失焦保流回归测试（GitHub issue #54，第二部分）。
//
// 背景：输入法失焦时应“保留音频管理器可见的采集流、但暂停识别”。真实
// `PipeWireCapture` 通过 SetDeliveryEnabled(bool) 打开/关闭交付闸门（epoch），
// 同时必须：
//   - 暂停期间不销毁 stream（音频管理器侧路由不中断）、不把音频放进队列；
//   - 恢复后只交付新样本（不重放失焦期间的旧样本），且不重开 stream；
//   - 一个已取 epoch、仍在写的旧 RT 回调跨快速 pause/resume 后不得泄漏旧样本；
//   - stream 进入 ERROR 后 IsRunning() 立即为 false，且 Stop() 仍释放全部资源。
//
// 测试边界（用户已确认）：真实 `PipeWireCapture::Start/Stop/SetDeliveryEnabled`
// + 测试专用假 libpipewire-0.3。测试二进制的父进程为每个场景 fork 一个子进程，
// 子进程带上指向构建目录假库的 `LD_LIBRARY_PATH`，从而在**真实 dlopen 路径**上
// 运行 capture，不新增生产依赖注入层，也不链接真实 libpipewire。
//
// 假库只写入构建目录，绝不安装或覆盖系统库；不接触真实 PipeWire 服务/设备/网络。

#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "capture/pipewire_capture.h"
#include "support/fake_pipewire.h"
#include "types.h"
#include "utils/thread_safe_queue.h"

namespace {

using namespace std::chrono_literals;

int g_failures = 0;

void Check(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

// 通过 dlopen/dlsym 获得的假库控制接口。测试可执行文件不链接假库。
struct FakePw {
    void* handle = nullptr;
    decltype(&fakepw_reset) reset = nullptr;
    decltype(&fakepw_stream_new_count) streamNew = nullptr;
    decltype(&fakepw_stream_destroy_count) streamDestroy = nullptr;
    decltype(&fakepw_stream_connect_count) streamConnect = nullptr;
    decltype(&fakepw_stream_queue_count) streamQueue = nullptr;
    decltype(&fakepw_core_disconnect_count) coreDisconnect = nullptr;
    decltype(&fakepw_context_destroy_count) contextDestroy = nullptr;
    decltype(&fakepw_thread_loop_start_count) loopStart = nullptr;
    decltype(&fakepw_thread_loop_stop_count) loopStop = nullptr;
    decltype(&fakepw_thread_loop_destroy_count) loopDestroy = nullptr;
    decltype(&fakepw_dequeue_count) dequeue = nullptr;
    decltype(&fakepw_set_sample_value) setSampleValue = nullptr;
    decltype(&fakepw_set_samples_per_buffer) setSamplesPerBuffer = nullptr;
    decltype(&fakepw_set_callback_interval_ms) setCallbackInterval = nullptr;
    decltype(&fakepw_hold_next_dequeue) holdNextDequeue = nullptr;
    decltype(&fakepw_dequeue_held) dequeueHeld = nullptr;
    decltype(&fakepw_release_held_dequeue) releaseHeldDequeue = nullptr;
    decltype(&fakepw_held_sample_value) heldSampleValue = nullptr;
    decltype(&fakepw_trigger_stream_error) triggerStreamError = nullptr;
    decltype(&fakepw_set_empty_mode) setEmptyMode = nullptr;
    decltype(&fakepw_deliver_one_valid_buffer) deliverOneValidBuffer = nullptr;
};

bool LoadFakePw(FakePw& fp) {
    fp.handle = dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!fp.handle) {
        std::cerr << "FAIL: dlopen fake libpipewire-0.3.so.0: " << dlerror() << '\n';
        return false;
    }

#define LOAD(field, symbol)                                                          \
    do {                                                                             \
        fp.field = reinterpret_cast<decltype(fp.field)>(dlsym(fp.handle, symbol));   \
        if (!fp.field) {                                                             \
            std::cerr << "FAIL: dlsym " symbol ": " << dlerror() << '\n';            \
            return false;                                                            \
        }                                                                            \
    } while (false)

    LOAD(reset, "fakepw_reset");
    LOAD(streamNew, "fakepw_stream_new_count");
    LOAD(streamDestroy, "fakepw_stream_destroy_count");
    LOAD(streamConnect, "fakepw_stream_connect_count");
    LOAD(streamQueue, "fakepw_stream_queue_count");
    LOAD(coreDisconnect, "fakepw_core_disconnect_count");
    LOAD(contextDestroy, "fakepw_context_destroy_count");
    LOAD(loopStart, "fakepw_thread_loop_start_count");
    LOAD(loopStop, "fakepw_thread_loop_stop_count");
    LOAD(loopDestroy, "fakepw_thread_loop_destroy_count");
    LOAD(dequeue, "fakepw_dequeue_count");
    LOAD(setSampleValue, "fakepw_set_sample_value");
    LOAD(setSamplesPerBuffer, "fakepw_set_samples_per_buffer");
    LOAD(setCallbackInterval, "fakepw_set_callback_interval_ms");
    LOAD(holdNextDequeue, "fakepw_hold_next_dequeue");
    LOAD(dequeueHeld, "fakepw_dequeue_held");
    LOAD(releaseHeldDequeue, "fakepw_release_held_dequeue");
    LOAD(heldSampleValue, "fakepw_held_sample_value");
    LOAD(triggerStreamError, "fakepw_trigger_stream_error");
    LOAD(setEmptyMode, "fakepw_set_empty_mode");
    LOAD(deliverOneValidBuffer, "fakepw_deliver_one_valid_buffer");
#undef LOAD
    return true;
}

// 与生产 DrainLoop 的 float→int16 转换保持一致。
int16_t Expected(float value) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int16_t>(clamped * 32767.0f);
}

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

bool PopFrame(ThreadSafeQueue<fcitx::AudioFrame>& queue, fcitx::AudioFrame& frame,
              std::chrono::milliseconds timeout = 2s) {
    return WaitFor([&] { return queue.TryPop(frame); }, timeout);
}

// 场景 1：Start 产帧；暂停不销毁 stream、不填队列；恢复只交付新值且不重开。
int RunLifecycle(FakePw& fp) {
    fp.reset();
    fp.setSampleValue(0.25f);

    ThreadSafeQueue<fcitx::AudioFrame> queue;
    fcitx::PipeWireCapture capture;
    capture.SetFrameQueue(&queue);

    Check(capture.Start(), "Start() succeeds");
    Check(capture.IsRunning(), "IsRunning() true after Start");
    Check(fp.streamNew() == 1, "pw_stream_new called once");

    fcitx::AudioFrame frame;
    Check(PopFrame(queue, frame), "capture produces frames");
    Check(frame.pcm[0] == Expected(0.25f), "frame carries fake PCM value");
    Check(frame.pcm[fcitx::kWindowSize - 1] == Expected(0.25f), "full window filled");

    // 暂停：闸门关闭，但仍持续消费 RT 回调。
    capture.SetDeliveryEnabled(false);
    queue.Clear();
    fp.setSampleValue(0.5f);
    const unsigned before = fp.dequeue();
    Check(WaitFor([&] { return fp.dequeue() >= before + 8; }, 2s),
          "capture keeps consuming while paused");
    Check(queue.Empty(), "paused capture discards audio rather than buffering it");
    Check(capture.IsRunning(), "pause retains the stream running");
    Check(fp.streamDestroy() == 0, "pause does not destroy the stream");
    Check(fp.streamNew() == 1, "pause does not reopen the stream");

    // 恢复：只应交付新值 0.75，且不重开 stream。
    fp.setSampleValue(0.75f);
    capture.SetDeliveryEnabled(true);
    unsigned delivered = 0;
    const auto resumeDeadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < resumeDeadline && delivered < 4) {
        if (PopFrame(queue, frame, 200ms)) {
            Check(frame.pcm[0] == Expected(0.75f), "resume contains only fresh audio");
            ++delivered;
        }
    }
    Check(delivered > 0, "delivery resumes without reopening capture");
    Check(fp.streamNew() == 1, "resume reuses the audio manager's stream");

    capture.Stop();
    Check(!capture.IsRunning(), "IsRunning() false after Stop");
    Check(fp.streamDestroy() == 1, "pw_stream_destroy called once on Stop");
    Check(fp.loopStop() == 1, "pw_thread_loop_stop called once");
    Check(fp.loopDestroy() == 1, "pw_thread_loop_destroy called once");
    Check(fp.coreDisconnect() == 1, "pw_core_disconnect called once");
    Check(fp.contextDestroy() == 1, "pw_context_destroy called once");
    return g_failures == 0 ? 0 : 1;
}

// 场景 2：旧 RT 回调（dequeue 被屏障阻塞、此前已取启用 epoch）跨快速
// pause/resume 后释放旧值，旧样本不得泄漏到恢复后的队列。
int RunStale(FakePw& fp) {
    fp.reset();
    fp.setSampleValue(0.25f);

    ThreadSafeQueue<fcitx::AudioFrame> queue;
    fcitx::PipeWireCapture capture;
    capture.SetFrameQueue(&queue);

    Check(capture.Start(), "Start() succeeds");
    fcitx::AudioFrame frame;
    Check(PopFrame(queue, frame), "capture produces frames");
    Check(frame.pcm[0] == Expected(0.25f), "frame carries the pre-pause value");

    // 武装屏障：下一个 RT 回调会阻塞在 dequeue（此刻交付仍启用，epoch 已取）。
    fp.holdNextDequeue();
    Check(WaitFor([&] { return fp.dequeueHeld() != 0; }, 2s),
          "RT callback blocked inside dequeue");

    // 快速 pause → resume，随后才放行旧回调。
    capture.SetDeliveryEnabled(false);
    queue.Clear();
    fp.setSampleValue(0.75f);
    capture.SetDeliveryEnabled(true);
    fp.releaseHeldDequeue();

    Check(fp.heldSampleValue() == 0.25f, "held dequeue snapshotted the old value");

    unsigned delivered = 0;
    const auto resumeDeadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < resumeDeadline && delivered < 8) {
        if (PopFrame(queue, frame, 200ms)) {
            Check(frame.pcm[0] == Expected(0.75f),
                  "stale samples did not leak after resume");
            ++delivered;
        }
    }
    Check(delivered > 0, "capture resumes after the stale callback");

    capture.Stop();
    Check(!capture.IsRunning(), "IsRunning() false after Stop");
    Check(fp.streamDestroy() == 1, "pw_stream_destroy called once on Stop");
    Check(fp.loopStop() == 1, "pw_thread_loop_stop called once");
    Check(fp.loopDestroy() == 1, "pw_thread_loop_destroy called once");
    return g_failures == 0 ? 0 : 1;
}

// 场景 3：stream ERROR 后 IsRunning() 立即为 false；Stop() 即使 running==false
// 仍释放全部资源。
int RunError(FakePw& fp) {
    fp.reset();
    fp.setSampleValue(0.25f);

    ThreadSafeQueue<fcitx::AudioFrame> queue;
    fcitx::PipeWireCapture capture;
    capture.SetFrameQueue(&queue);

    Check(capture.Start(), "Start() succeeds");
    fcitx::AudioFrame frame;
    Check(PopFrame(queue, frame), "capture produces frames");

    fp.triggerStreamError("fake stream error");
    Check(WaitFor([&] { return !capture.IsRunning(); }, 2s),
          "stream error clears IsRunning");
    Check(fp.streamDestroy() == 0, "error alone does not destroy before Stop");

    capture.Stop();
    Check(!capture.IsRunning(), "IsRunning() false after Stop");
    Check(fp.streamDestroy() == 1, "Stop after error still frees the stream");
    Check(fp.loopStop() == 1, "pw_thread_loop_stop called once");
    Check(fp.loopDestroy() == 1, "pw_thread_loop_destroy called once");
    Check(fp.coreDisconnect() == 1, "pw_core_disconnect called once");
    Check(fp.contextDestroy() == 1, "pw_context_destroy called once");
    return g_failures == 0 ? 0 : 1;
}

int RunEmptyBuffers(FakePw& fp) {
    fp.reset();
    ThreadSafeQueue<fcitx::AudioFrame> queue;
    fcitx::PipeWireCapture capture;
    capture.SetFrameQueue(&queue);
    capture.SetDeliveryEnabled(false);
    Check(capture.Start(), "Start with muted delivery");
    for (unsigned mode = 1; mode <= 4; ++mode) {
        capture.SetDeliveryEnabled(false);
        queue.Clear();
        fp.setEmptyMode(mode);
        fp.setSampleValue(0.5f);
        capture.SetDeliveryEnabled(true);
        const auto before = fp.dequeue();
        Check(WaitFor([&] { return fp.dequeue() >= before + 10; }, 2s),
              "empty callbacks complete before fresh audio");
        // One valid frame, then empty callbacks again: it must not be lost
        // merely because the producer failed to acknowledge an empty callback.
        fp.deliverOneValidBuffer();
        fcitx::AudioFrame frame;
        const bool delivered = PopFrame(queue, frame, 200ms);
        Check(delivered, "first valid frame after empty callbacks is delivered (mode " +
                             std::to_string(mode) + ")");
        if (delivered) Check(frame.pcm[0] == Expected(0.5f), "fresh buffer content");
    }
    capture.Stop();
    return g_failures ? 1 : 0;
}

int RunScenario(const std::string& scenario, FakePw& fp) {
    if (scenario == "lifecycle") return RunLifecycle(fp);
    if (scenario == "stale") return RunStale(fp);
    if (scenario == "error") return RunError(fp);
    if (scenario == "empty") return RunEmptyBuffers(fp);
    std::cerr << "FAIL: unknown scenario '" << scenario << "'\n";
    return 2;
}

std::string SelfExe() {
    char buffer[4096];
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) return {};
    buffer[length] = '\0';
    return buffer;
}

std::string EnvOrEmpty(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

// 父进程：为每个场景 fork/exec 自身作为子进程，注入假库搜索路径。
int RunParent(const std::string& fakeLibDir) {
    const std::string exe = SelfExe();
    Check(!exe.empty(), "resolve /proc/self/exe");

    const std::string fakeLib = fakeLibDir + "/libpipewire-0.3.so.0";
    Check(access(fakeLib.c_str(), R_OK) == 0, "fake libpipewire-0.3.so.0 exists: " + fakeLib);

    const std::string childLd =
        fakeLibDir + (EnvOrEmpty("LD_LIBRARY_PATH").empty()
                          ? ""
                          : ":" + EnvOrEmpty("LD_LIBRARY_PATH"));

    for (const char* scenario : {"lifecycle", "stale", "error", "empty"}) {
        const pid_t pid = fork();
        if (pid == 0) {
            setenv("LD_LIBRARY_PATH", childLd.c_str(), 1);
            execl(exe.c_str(), exe.c_str(), "child", scenario, static_cast<char*>(nullptr));
            std::perror("execl");
            _exit(127);
        }
        if (pid < 0) {
            Check(false, "fork failed");
            continue;
        }

        int status = 0;
        waitpid(pid, &status, 0);
        Check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              std::string("scenario '") + scenario + "' passed in subprocess");
    }

    if (g_failures == 0) {
        std::cout << "pipewire capture lifecycle: all scenarios passed\n";
    }
    return g_failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "child") == 0) {
        FakePw fp;
        if (!LoadFakePw(fp)) return 2;
        return RunScenario(argv[2], fp);
    }
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <fake-lib-dir>\n";
        return 2;
    }
    return RunParent(argv[1]);
}
