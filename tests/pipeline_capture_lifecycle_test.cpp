#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcitx-utils/log.h>
#include <unistd.h>

#include "pipeline/pipeline.h"
#include "support/fake_pulse.h"

using namespace std::chrono_literals;

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename Predicate>
bool WaitFor(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// fcitx's log stream is global and setLogStream is not thread safe. Callers
// redirect only while no pipeline thread is alive, and restore it on scope exit.
class LogFileCapture {
public:
    explicit LogFileCapture(const char* name) {
        const auto pattern = (std::filesystem::temp_directory_path() /
                              (std::string("voice-input-") + name + "-XXXXXX"))
                                 .string();
        std::vector<char> path(pattern.begin(), pattern.end());
        path.push_back('\0');
        const int fd = ::mkstemp(path.data());
        if (fd < 0) throw std::runtime_error("failed to create temporary log file");
        ::close(fd);
        path_ = path.data();
        // mkstemp created an empty unique file; append avoids an implicit truncate.
        stream_.open(path_, std::ios::out | std::ios::app);
        if (!stream_.is_open()) {
            std::error_code error;
            std::filesystem::remove(path_, error);
            throw std::runtime_error("failed to open temporary log file");
        }
    }

    ~LogFileCapture() {
        Restore();
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    void Begin(bool debug) {
        fcitx::Log::setLogRule(debug ? "default=5" : "");
        fcitx::Log::setLogStream(stream_);
        active_ = true;
    }

    std::string EndAndRead() {
        Restore();
        stream_.flush();
        std::ifstream input(path_);
        if (!input.is_open()) throw std::runtime_error("failed to read log file");
        return {std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
    }

private:
    void Restore() {
        if (!active_) return;
        active_ = false;
        fcitx::Log::setLogStream(std::cerr);
        fcitx::Log::setLogRule("");
    }

    std::filesystem::path path_;
    std::ofstream stream_;
    bool active_ = false;
};

size_t FindTiming(const std::string& log, std::string_view text,
                  size_t from, const char* message) {
    const auto pos = log.find(text, from);
    if (pos == std::string::npos) throw std::runtime_error(message);
    return pos;
}

std::string TimingLine(const std::string& log, size_t pos) {
    const auto end = log.find('\n', pos);
    return log.substr(pos, end == std::string::npos ? end : end - pos);
}

int NumberAfter(const std::string& line, std::string_view field) {
    const auto pos = line.find(field);
    if (pos == std::string::npos) throw std::runtime_error("missing timing field");
    return std::stoi(line.substr(pos + field.size()));
}

struct RecognitionLog {
    std::atomic<unsigned> sessions{0};
    std::atomic<unsigned> cancellations{0};
    std::mutex mutex;
    std::vector<int> samples;

    size_t Size() {
        std::lock_guard<std::mutex> lock(mutex);
        return samples.size();
    }
};

class TestSession final : public fcitx::AsrSession {
public:
    TestSession(uint64_t id, RecognitionLog& log) : log_(log) {
        state_->sessionId = id;
    }
    void FeedAudio(const float* pcm, size_t frames) override {
        std::lock_guard<std::mutex> lock(log_.mutex);
        for (size_t i = 0; i < frames; ++i) {
            log_.samples.push_back(static_cast<int>(std::lround(pcm[i] * 32768)));
        }
    }
    void StartWorker() override {}
    void End() override { state_->finished = true; }
    void JoinWithTimeout(std::chrono::milliseconds) override {}
    void Cancel() override {
        if (!state_->cancelled.exchange(true)) ++log_.cancellations;
    }

private:
    RecognitionLog& log_;
};

class TestEngine final : public fcitx::AsrEngine {
public:
    explicit TestEngine(RecognitionLog& log) : log_(log) {}
    bool Init(const Config&) override { return true; }
    const char* Name() const override { return "test-recognizer"; }
    fcitx::AsrSessionStart StartSession() override {
        auto session = std::make_shared<TestSession>(NextSessionId(), log_);
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions_[session->GetState()->sessionId] = session;
        ++log_.sessions;
        return {session, std::nullopt};
    }

private:
    RecognitionLog& log_;
};

void VerifyInfoLog(const std::string& log) {
    Require(log.find("Pipeline started") != std::string::npos,
            "INFO log capture received no pipeline logs");
    Require(log.find("[voice-input:timing] ") == std::string::npos,
            "timing logs appeared with the default INFO log level");
}

void VerifyDebugLog(const std::string& log) {
    const auto startBegin = FindTiming(
        log, "stage=capture.start backend=pulseaudio event=begin", 0,
        "capture.start begin log is missing");
    const auto startEnd = FindTiming(
        log, "stage=capture.start backend=pulseaudio event=end elapsed_ms=",
        startBegin + 1, "capture.start end is missing or out of order");
    const auto pipelineStart = FindTiming(
        log, "stage=pipeline.start event=end capture_prepare_ms=", startEnd + 1,
        "pipeline.start end is missing or out of order");
    const auto pauseEnd = FindTiming(
        log, "stage=pipeline.pause event=end gate_ms=", pipelineStart + 1,
        "pipeline.pause end is missing or out of order");
    const auto stopBegin = FindTiming(
        log, "stage=capture.stop backend=pulseaudio event=begin", pauseEnd + 1,
        "capture.stop begin is missing or out of order");
    const auto stopEnd = FindTiming(
        log, "stage=capture.stop backend=pulseaudio event=end elapsed_ms=",
        stopBegin + 1, "capture.stop end is missing or out of order");

    Require(TimingLine(log, startEnd).ends_with(" success=1"),
            "capture.start end is missing success=1");
    const auto start = TimingLine(log, pipelineStart);
    Require(start.find(" vad_ms=") != std::string::npos &&
                start.find(" total_ms=") != std::string::npos &&
                start.ends_with(" success=1"),
            "pipeline.start end fields are incomplete");
    const auto pause = TimingLine(log, pauseEnd);
    Require(pause.find(" results_ms=") != std::string::npos &&
                pause.find(" vad_ms=") != std::string::npos &&
                pause.find(" dispatcher_ms=") != std::string::npos &&
                pause.find(" cleanup_ms=") != std::string::npos &&
                pause.find(" total_ms=") != std::string::npos,
            "pipeline.pause end fields are incomplete");

    // The fake read sleeps for 300 ms. Stop joins it, so the measured duration
    // must be clearly above the default 2 ms fake read.
    Require(NumberAfter(TimingLine(log, stopEnd), "elapsed_ms=") >= 100,
            "capture.stop did not account for the delayed fake read");
}

} // namespace

// Replace only the model boundary: every input frame produces speech events.
// Capture, Pipeline, queues, dispatcher, session cancellation and reaper are real.
void fcitx::VADWorker::Start() {
    if (running_) return;
    running_ = true;
    thread_ = std::make_unique<std::thread>([this] {
        bool started = false;
        while (running_) {
            AudioFrame frame;
            if (!frameQueue_->TryPop(frame)) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            if (!started) {
                SpeechEvent begin;
                begin.type = SpeechEventType::Begin;
                speechEventQueue_->Push(std::move(begin));
                started = true;
            }
            SpeechEvent audio;
            audio.type = SpeechEventType::Audio;
            audio.pcm.assign(frame.pcm.begin(), frame.pcm.end());
            speechEventQueue_->Push(std::move(audio));
        }
    });
}

int main() {
    try {
        std::string infoLog;
        {
            LogFileCapture capture("pipeline-capture-info");
            capture.Begin(false);
            fakepulse_reset();
            fakepulse_set_read_sample_value(1111);
            RecognitionLog log;
            {
                fcitx::Pipeline pipeline;
                pipeline.Init(fcitx::VoiceInputConfig{});
                pipeline.SetAsrEngine(std::make_unique<TestEngine>(log));
                pipeline.SetGeneration(1);
                pipeline.Start();
                Require(WaitFor([&] { return log.Size() > 0; }),
                        "initial audio was not recognized");
                Require(pipeline.IsRunning() && pipeline.IsCapturing(),
                        "initial start failed");

                pipeline.Pause();
                Require(!pipeline.IsRunning() && pipeline.IsCapturing(),
                        "focus pause must retain capture but stop recognition");
                Require(fakepulse_free_count() == 0, "pause freed the stream");
                Require(log.cancellations == 1, "pause did not cancel the session");
                const auto pausedSamples = log.Size();
                const auto pausedSessions = log.sessions.load();
                const auto pausedReads = fakepulse_read_count();
                fakepulse_set_read_sample_value(2222);
                Require(WaitFor([&] {
                            return fakepulse_read_count() >= pausedReads + 8;
                        }),
                        "capture stopped consuming audio while paused");
                Require(log.Size() == pausedSamples &&
                            log.sessions == pausedSessions,
                        "unfocused audio reached recognition");

                fakepulse_set_read_sample_value(3333);
                pipeline.SetGeneration(3);
                pipeline.Start();
                Require(WaitFor([&] { return log.Size() > pausedSamples; }),
                        "recognition did not resume");
                Require(fakepulse_new_count() == 1, "resume reopened capture");
                pipeline.Pause();
                {
                    std::lock_guard<std::mutex> lock(log.mutex);
                    for (size_t i = pausedSamples; i < log.samples.size(); ++i) {
                        Require(log.samples[i] == 3333, "resume replayed old audio");
                    }
                }
                // A stream retained during focus loss can fail independently of
                // the stopped recognition workers. Resume must recreate it.
                fakepulse_set_fail_read(1);
                Require(WaitFor([&] { return !pipeline.IsCapturing(); }),
                        "read failure was not reflected in capture liveness");
                fakepulse_set_fail_read(0);
                pipeline.SetGeneration(5);
                const auto beforeRecovery = log.Size();
                pipeline.Start();
                Require(WaitFor([&] { return log.Size() > beforeRecovery; }),
                        "resume did not recover the failed stream");
                Require(fakepulse_new_count() == 2 && fakepulse_free_count() == 1,
                        "failed stream was not replaced exactly once");

                pipeline.Stop();
                Require(!pipeline.IsCapturing() && !pipeline.IsRunning(),
                        "close did not stop capture");
                Require(fakepulse_free_count() == 2, "close did not free the stream");
                pipeline.Stop();
                Require(fakepulse_free_count() == 2, "repeated close freed twice");
            }
            infoLog = capture.EndAndRead();
        }
        VerifyInfoLog(infoLog);

        // DEBUG timing must include the capture.stop duration of an in-flight read.
        std::string debugLog;
        {
            LogFileCapture capture("pipeline-capture-debug");
            capture.Begin(true);
            fakepulse_reset();
            fakepulse_set_read_sample_value(4444);
            RecognitionLog log;
            {
                fcitx::Pipeline pipeline;
                pipeline.Init(fcitx::VoiceInputConfig{});
                pipeline.SetAsrEngine(std::make_unique<TestEngine>(log));
                pipeline.SetGeneration(7);
                pipeline.Start();
                Require(WaitFor([&] { return fakepulse_read_count() > 0; }),
                        "DEBUG start did not begin capture reads");

                fakepulse_set_read_delay_ms(300);
                const auto readsBeforeStop = fakepulse_read_count();
                Require(WaitFor([&] {
                            return fakepulse_read_count() > readsBeforeStop;
                        }),
                        "capture read did not start before stop");
                pipeline.Stop();
                Require(!pipeline.IsCapturing() && !pipeline.IsRunning(),
                        "DEBUG stop did not stop capture");
            }
            debugLog = capture.EndAndRead();
        }
        VerifyDebugLog(debugLog);

        std::cout << "PASS: focus pause retains capture without recognition or replay; "
                     "stage timing logs are DEBUG-only and ordered\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
