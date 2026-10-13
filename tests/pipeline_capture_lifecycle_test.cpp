#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

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
        fakepulse_reset();
        fakepulse_set_read_sample_value(1111);
        RecognitionLog log;
        fcitx::Pipeline pipeline;
        pipeline.Init(fcitx::VoiceInputConfig{});
        pipeline.SetAsrEngine(std::make_unique<TestEngine>(log));
        pipeline.SetGeneration(1);
        pipeline.Start();
        Require(WaitFor([&] { return log.Size() > 0; }), "initial audio was not recognized");
        Require(pipeline.IsRunning() && pipeline.IsCapturing(), "initial start failed");

        pipeline.Pause();
        Require(!pipeline.IsRunning() && pipeline.IsCapturing(),
                "focus pause must retain capture but stop recognition");
        Require(fakepulse_free_count() == 0, "pause freed the audio-manager stream");
        Require(log.cancellations == 1, "pause did not cancel the active session");
        const auto pausedSamples = log.Size();
        const auto pausedSessions = log.sessions.load();
        const auto pausedReads = fakepulse_read_count();
        fakepulse_set_read_sample_value(2222);
        Require(WaitFor([&] { return fakepulse_read_count() >= pausedReads + 8; }),
                "capture stopped consuming audio while paused");
        Require(log.Size() == pausedSamples && log.sessions == pausedSessions,
                "unfocused audio reached recognition");

        fakepulse_set_read_sample_value(3333);
        pipeline.SetGeneration(3);
        pipeline.Start();
        Require(WaitFor([&] { return log.Size() > pausedSamples; }), "recognition did not resume");
        Require(fakepulse_new_count() == 1, "focus resume reopened capture");
        pipeline.Pause();
        {
            std::lock_guard<std::mutex> lock(log.mutex);
            for (size_t i = pausedSamples; i < log.samples.size(); ++i) {
                Require(log.samples[i] == 3333, "resume replayed unfocused/old audio");
            }
        }
        // A stream retained during focus loss can fail independently of the
        // stopped recognition workers. Resume must recreate it, not stay mute.
        fakepulse_set_fail_read(1);
        Require(WaitFor([&] { return !pipeline.IsCapturing(); }),
                "read failure was not reflected in capture liveness");
        fakepulse_set_fail_read(0);
        pipeline.SetGeneration(5);
        const auto beforeRecovery = log.Size();
        pipeline.Start();
        Require(WaitFor([&] { return log.Size() > beforeRecovery; }),
                "resume did not recover the failed retained stream");
        Require(fakepulse_new_count() == 2 && fakepulse_free_count() == 1,
                "failed retained stream was not replaced exactly once");

        pipeline.Stop();
        Require(!pipeline.IsCapturing() && !pipeline.IsRunning(), "close did not stop capture");
        Require(fakepulse_free_count() == 2, "close did not free the recovered stream");
        pipeline.Stop();
        Require(fakepulse_free_count() == 2, "repeated close freed the stream twice");
        std::cout << "PASS: focus pause retains capture without recognition or replay\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
