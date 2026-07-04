#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "types.h"
#include "utils/thread_safe_queue.h"

namespace fcitx {

class SileroVad;

class VADWorker {
public:
    // VAD tuning parameters
    struct Config {
        float speechThreshold = 0.5f;
        float silenceThreshold = 0.35f;
        int startFrames = 2;           // consecutive speech frames to trigger onset
        int preRollMs = 300;           // audio before onset to include
        int endSilenceMs = 700;        // silence before ending utterance
        int minSpeechMs = 300;         // minimum utterance duration
        int maxSpeechMs = 30000;       // maximum utterance duration
        std::string sileroModelPath;   // empty = installed default
    };

    VADWorker();
    ~VADWorker();

    VADWorker(const VADWorker&) = delete;
    VADWorker& operator=(const VADWorker&) = delete;

    using VadStatusCallback = std::function<void(bool speaking)>;

    void SetConfig(const Config& config);
    void SetFrameQueue(ThreadSafeQueue<AudioFrame>* queue);
    void SetSpeechEventQueue(ThreadSafeQueue<SpeechEvent>* queue);
    void SetVadStatusCallback(VadStatusCallback cb);

    void Start();
    void Stop();

    bool IsRunning() const { return running_.load(); }

private:
    void WorkerLoop();
    void ProcessFrame(const AudioFrame& frame, float probability);
    void AppendPreRoll(const std::array<int16_t, kWindowSize>& pcm);
    void ResetSession();

    Config config_;

    std::unique_ptr<SileroVad> silero_;

    ThreadSafeQueue<AudioFrame>* frameQueue_ = nullptr;
    ThreadSafeQueue<SpeechEvent>* speechEventQueue_ = nullptr;

    std::unique_ptr<std::thread> thread_;
    std::atomic<bool> running_{false};

    // Callback
    VadStatusCallback vadStatusCb_;

    // Session state
    enum class State { Idle, Speaking };
    State state_ = State::Idle;

    std::deque<int16_t> preRoll_;

    int speechFrames_ = 0;
    int silenceFrames_ = 0;
    int64_t startMs_ = 0;
    int64_t lastSpeechMs_ = 0;
};

} // namespace fcitx
