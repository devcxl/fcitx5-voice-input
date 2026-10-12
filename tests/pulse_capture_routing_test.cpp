// PulseAudio 录音源路由回归测试（GitHub issue #54）。
//
// 背景：`pulse_audio_capture.cpp` 曾用 `pactl list sources short` 枚举录音源，
// 再用名称启发式（跳过 `.monitor` / `echoCancel`、优先 `alsa_input.*Mic1*`）
// 猜出一个源名，并把它显式传给 `pa_simple_new` 的 `dev` 参数。这把一次单机
// 异常（echoCancel 无声）固化成全局设备策略，越过了音频服务/管理器的默认源
// 与按应用路由恢复机制：内置麦克风排在 USB 之前时会被选中，即使它不是用户
// 期望的设备（#54 现象）。
//
// 修复方向：删除猜源，`dev = nullptr`，把路由交回音频服务与音频管理器。
//
// 测试边界（用户已确认）：真实 `PulseAudioCapture::Start/Stop` + 测试专用假
// libpulse-simple。测试二进制的父进程为每个场景 fork 出一个子进程，子进程带上
// 指向构建目录假库的 `LD_LIBRARY_PATH` 与指向假 `pactl` 的 `PATH`，从而在
// **真实 dlopen 路径**上运行 capture，不新增生产依赖注入层。
//
// 断言：
//   1. `dev == nullptr`（委托路由），不调用 `pactl`（无子进程、无标记文件）；
//   2. 应用名 `fcitx5-voice-input`、流名 `voice input` 不变；
//   3. `PA_STREAM_RECORD`、S16LE、16kHz、mono；
//   4. read 产帧进入队列，在有限时间内 Stop（join 有界）、`pa_simple_free` 恰好一次；
//   5. 建流失败时 Start 返回 false 且不 free 未创建的流。
//
// 假库只写入构建目录，绝不安装或覆盖系统库；不接触真实音频服务/设备/网络。

#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "capture/pulse_audio_capture.h"
#include "support/fake_pulse.h"
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
struct FakePulse {
    void* handle = nullptr;
    decltype(&fakepulse_reset) reset = nullptr;
    decltype(&fakepulse_new_count) newCount = nullptr;
    decltype(&fakepulse_free_count) freeCount = nullptr;
    decltype(&fakepulse_read_count) readCount = nullptr;
    decltype(&fakepulse_dev_is_null) devIsNull = nullptr;
    decltype(&fakepulse_app_name) appName = nullptr;
    decltype(&fakepulse_stream_name) streamName = nullptr;
    decltype(&fakepulse_stream_direction) streamDirection = nullptr;
    decltype(&fakepulse_have_sample_spec) haveSampleSpec = nullptr;
    decltype(&fakepulse_sample_format) sampleFormat = nullptr;
    decltype(&fakepulse_sample_rate) sampleRate = nullptr;
    decltype(&fakepulse_channels) channels = nullptr;
    decltype(&fakepulse_set_fail_new) setFailNew = nullptr;
    decltype(&fakepulse_set_read_sample_value) setReadSampleValue = nullptr;
    decltype(&fakepulse_set_read_delay_ms) setReadDelayMs = nullptr;
};

bool LoadFakePulse(FakePulse& fp) {
    fp.handle = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!fp.handle) {
        std::cerr << "FAIL: dlopen fake libpulse-simple.so.0: " << dlerror() << '\n';
        return false;
    }

#define LOAD(field, symbol)                                                     \
    do {                                                                        \
        fp.field = reinterpret_cast<decltype(fp.field)>(dlsym(fp.handle, symbol)); \
        if (!fp.field) {                                                        \
            std::cerr << "FAIL: dlsym " symbol ": " << dlerror() << '\n';       \
            return false;                                                       \
        }                                                                       \
    } while (false)

    LOAD(reset, "fakepulse_reset");
    LOAD(newCount, "fakepulse_new_count");
    LOAD(freeCount, "fakepulse_free_count");
    LOAD(readCount, "fakepulse_read_count");
    LOAD(devIsNull, "fakepulse_dev_is_null");
    LOAD(appName, "fakepulse_app_name");
    LOAD(streamName, "fakepulse_stream_name");
    LOAD(streamDirection, "fakepulse_stream_direction");
    LOAD(haveSampleSpec, "fakepulse_have_sample_spec");
    LOAD(sampleFormat, "fakepulse_sample_format");
    LOAD(sampleRate, "fakepulse_sample_rate");
    LOAD(channels, "fakepulse_channels");
    LOAD(setFailNew, "fakepulse_set_fail_new");
    LOAD(setReadSampleValue, "fakepulse_set_read_sample_value");
    LOAD(setReadDelayMs, "fakepulse_set_read_delay_ms");
#undef LOAD
    return true;
}

int RunChild(const std::string& scenario) {
    FakePulse fp;
    if (!LoadFakePulse(fp)) return 2;

    fp.reset();
    fp.setReadDelayMs(2);
    fp.setReadSampleValue(0x1234);

    ThreadSafeQueue<fcitx::AudioFrame> queue;
    fcitx::PulseAudioCapture capture;
    capture.SetFrameQueue(&queue);

    if (scenario == "fail-new") {
        fp.setFailNew(1);
        Check(!capture.Start(), "Start() returns false when pa_simple_new fails");
        Check(fp.newCount() == 1, "pa_simple_new attempted exactly once");
        Check(fp.freeCount() == 0, "no pa_simple_free for a stream that was never created");
        Check(!capture.IsRunning(), "capture not running after failed Start");
        return g_failures == 0 ? 0 : 1;
    }

    if (scenario != "success" && scenario != "pause" && scenario != "rapid") {
        std::cerr << "FAIL: unknown scenario '" << scenario << "'\n";
        return 2;
    }

    Check(capture.Start(), "Start() succeeds");
    Check(capture.IsRunning(), "IsRunning() true after Start");
    Check(fp.newCount() == 1, "pa_simple_new called once");
    Check(fp.devIsNull() == 1,
          "dev == nullptr (routing delegated to the audio service)");
    Check(std::strcmp(fp.appName(), "fcitx5-voice-input") == 0,
          "application name unchanged");
    Check(std::strcmp(fp.streamName(), "voice input") == 0, "stream name unchanged");
    Check(fp.streamDirection() == PA_STREAM_RECORD, "record stream direction");
    Check(fp.haveSampleSpec() == 1, "sample spec provided");
    Check(fp.sampleFormat() == PA_SAMPLE_S16LE, "S16LE sample format");
    Check(fp.sampleRate() == 16000, "16kHz sample rate");
    Check(fp.channels() == 1, "mono channels");

    fcitx::AudioFrame frame;
    bool gotFrame = false;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (queue.TryPop(frame)) {
            gotFrame = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    Check(gotFrame, "capture thread pushes frames onto the queue");
    if (gotFrame) {
        Check(frame.pcm[0] == 0x1234, "frame carries pa_simple_read data");
        Check(frame.pcm[fcitx::kWindowSize - 1] == 0x1234, "full 512-sample window filled");
    }
    Check(fp.readCount() >= 1, "pa_simple_read was called");

    if (scenario == "pause" || scenario == "rapid") {
        if (scenario == "rapid") {
            fp.setReadDelayMs(40);
            const auto reads = fp.readCount();
            const auto readDeadline = std::chrono::steady_clock::now() + 2s;
            while (fp.readCount() == reads && std::chrono::steady_clock::now() < readDeadline) {
                std::this_thread::sleep_for(1ms);
            }
            Check(fp.readCount() > reads, "a blocking read is in flight before rapid pause/resume");
        }
        capture.SetDeliveryEnabled(false);
        queue.Clear();
        const auto pausedReads = fp.readCount();
        const auto pauseDeadline = std::chrono::steady_clock::now() + 2s;
        while (scenario == "pause" && fp.readCount() < pausedReads + 3 &&
               std::chrono::steady_clock::now() < pauseDeadline) {
            std::this_thread::sleep_for(1ms);
        }
        if (scenario == "pause") {
            Check(fp.readCount() >= pausedReads + 3, "paused capture keeps reading");
        }
        Check(queue.Empty(), "paused capture discards audio rather than buffering it");
        Check(fp.newCount() == 1 && fp.freeCount() == 0, "pause retains the same stream");
        fp.setReadSampleValue(0x2345);
        capture.SetDeliveryEnabled(true);
        bool resumed = false;
        const auto resumeDeadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < resumeDeadline) {
            if (queue.TryPop(frame)) {
                resumed = true;
                Check(frame.pcm[0] == 0x2345, "resume contains only fresh audio");
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        Check(resumed, "delivery resumes without reopening capture");
        Check(fp.newCount() == 1, "resume reuses the audio manager's stream");
    }

    capture.Stop();
    Check(!capture.IsRunning(), "IsRunning() false after Stop");
    Check(fp.freeCount() == 1, "pa_simple_free called once on Stop");

    return g_failures == 0 ? 0 : 1;
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

// 父进程：为每个场景 fork/exec 自身作为子进程，注入假库与假 pactl 的搜索路径。
int RunParent(const std::string& fakeLibDir, const std::string& fakePactlDir) {
    const std::string exe = SelfExe();
    Check(!exe.empty(), "resolve /proc/self/exe");

    const std::string fakeLib = fakeLibDir + "/libpulse-simple.so.0";
    Check(access(fakeLib.c_str(), R_OK) == 0, "fake libpulse-simple.so.0 exists: " + fakeLib);
    const std::string fakePactl = fakePactlDir + "/pactl";
    Check(access(fakePactl.c_str(), X_OK) == 0, "fake pactl exists: " + fakePactl);

    const std::string marker = fakePactlDir + "/pactl-called.marker";
    const std::string childLd =
        fakeLibDir + (EnvOrEmpty("LD_LIBRARY_PATH").empty()
                          ? ""
                          : ":" + EnvOrEmpty("LD_LIBRARY_PATH"));
    const std::string childPath =
        fakePactlDir +
        (EnvOrEmpty("PATH").empty() ? "" : ":" + EnvOrEmpty("PATH"));

    for (const char* scenario : {"success", "fail-new", "pause", "rapid"}) {
        std::remove(marker.c_str());

        const pid_t pid = fork();
        if (pid == 0) {
            setenv("LD_LIBRARY_PATH", childLd.c_str(), 1);
            setenv("PATH", childPath.c_str(), 1);
            setenv("PULSE_FAKE_PACTL_MARKER", marker.c_str(), 1);
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
        Check(access(marker.c_str(), F_OK) != 0,
              std::string("no pactl subprocess invoked (scenario '") + scenario + "')");
    }

    if (g_failures == 0) {
        std::cout << "pulse capture routing: all scenarios passed\n";
    }
    return g_failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "child") == 0) {
        return RunChild(argv[2]);
    }
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <fake-lib-dir> <fake-pactl-dir>\n";
        return 2;
    }
    return RunParent(argv[1], argv[2]);
}
