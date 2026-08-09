#include "vad.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <fcitx-utils/log.h>

#include "silero_vad.h"

namespace fcitx {

namespace {

std::string DefaultSileroModelPath() {
    return std::string(VOICE_INPUT_MODEL_DIR) + "/silero_vad.onnx";
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

size_t PreRollSamples(int preRollMs) {
    return static_cast<size_t>(kSampleRate) * preRollMs / 1000;
}

} // namespace

VADWorker::VADWorker() = default;

VADWorker::~VADWorker() {
    Stop();
}

void VADWorker::SetConfig(const Config& config) {
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        config_ = config;
    }

    FCITX_INFO() << "[voice-input:vadworker] Config:"
                 << " speechThresh=" << config.speechThreshold
                 << " silenceThresh=" << config.silenceThreshold
                 << " startFrames=" << config.startFrames
                 << " preRollMs=" << config.preRollMs
                 << " endSilenceMs=" << config.endSilenceMs
                 << " minSpeechMs=" << config.minSpeechMs
                 << " maxSpeechMs=" << config.maxSpeechMs;
}

void VADWorker::SetFrameQueue(ThreadSafeQueue<AudioFrame>* queue) {
    frameQueue_ = queue;
}

void VADWorker::SetSpeechEventQueue(ThreadSafeQueue<SpeechEvent>* queue) {
    speechEventQueue_ = queue;
}

void VADWorker::SetVadStatusCallback(VadStatusCallback cb) {
    vadStatusCb_ = std::move(cb);
}

void VADWorker::Start() {
    if (running_) return;

    // Init Silero
    std::string modelPath = config_.sileroModelPath.empty()
                                ? DefaultSileroModelPath()
                                : config_.sileroModelPath;
    silero_ = std::make_unique<SileroVad>(modelPath);
    if (!silero_->IsReady()) {
        FCITX_ERROR() << "[voice-input:vadworker] SileroVad init failed";
        return;
    }

    ResetSession();
    running_ = true;
    thread_ = std::make_unique<std::thread>(&VADWorker::WorkerLoop, this);
    FCITX_INFO() << "[voice-input:vadworker] Started";
}

void VADWorker::Stop() {
    if (!running_) return;
    running_ = false;
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
    thread_.reset();
    silero_.reset();
    FCITX_INFO() << "[voice-input:vadworker] Stopped";
}

void VADWorker::WorkerLoop() {
    while (running_) {
        AudioFrame frame;

        if (!frameQueue_ || !frameQueue_->TryPop(frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        // 快照配置：SetConfig 可能在主线程并发改写 config_
        Config config;
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            config = config_;
        }

        float prob = silero_->Predict(frame.pcm.data(), frame.pcm.size());
        if (prob < 0.0f) {
            // Inference failed
            continue;
        }

        ProcessFrame(frame, prob, config);
    }
}

void VADWorker::ProcessFrame(const AudioFrame& frame, float probability,
                             const Config& config) {
    bool speechStart = probability >= config.speechThreshold;
    bool speechKeep = probability >= config.silenceThreshold;

    AppendPreRoll(frame.pcm, PreRollSamples(config.preRollMs));

    if (state_ == State::Idle) {
        if (speechStart) {
            speechFrames_++;
            if (speechFrames_ >= config.startFrames) {
                state_ = State::Speaking;
                startMs_ = frame.timestamp_ms - config.preRollMs;

                SpeechEvent begin;
                begin.type = SpeechEventType::Begin;
                begin.timestamp_ms = startMs_;
                if (speechEventQueue_) speechEventQueue_->Push(std::move(begin));

                if (!preRoll_.empty()) {
                    SpeechEvent preAudio;
                    preAudio.type = SpeechEventType::Audio;
                    preAudio.timestamp_ms = startMs_;
                    preAudio.pcm.assign(preRoll_.begin(), preRoll_.end());
                    if (speechEventQueue_) speechEventQueue_->Push(std::move(preAudio));
                }

                SpeechEvent audio;
                audio.type = SpeechEventType::Audio;
                audio.timestamp_ms = frame.timestamp_ms;
                audio.pcm.assign(frame.pcm.begin(), frame.pcm.end());
                if (speechEventQueue_) speechEventQueue_->Push(std::move(audio));

                silenceFrames_ = 0;
                lastSpeechMs_ = frame.timestamp_ms;
                speechFrames_ = 0;

                FCITX_INFO() << "[voice-input:vadworker] Speech onset"
                             << " startMs=" << startMs_
                             << " preRollSamples=" << preRoll_.size();
                if (vadStatusCb_) {
                    vadStatusCb_(true);
                }
            }
        } else {
            speechFrames_ = 0;
        }
        return;
    }

    // State::Speaking
    SpeechEvent audio;
    audio.type = SpeechEventType::Audio;
    audio.timestamp_ms = frame.timestamp_ms;
    audio.pcm.assign(frame.pcm.begin(), frame.pcm.end());
    if (speechEventQueue_) speechEventQueue_->Push(std::move(audio));

    if (speechKeep) {
        silenceFrames_ = 0;
        lastSpeechMs_ = frame.timestamp_ms;
    } else {
        silenceFrames_++;
    }

    int endSilenceFrames =
        config.endSilenceMs / kFrameMs;
    bool silenceEnd = silenceFrames_ >= endSilenceFrames;

    int maxDurationMs = config.maxSpeechMs;
    bool tooLong =
        (lastSpeechMs_ - startMs_) >= maxDurationMs;

    if (silenceEnd || tooLong) {
        int durationMs = static_cast<int>((lastSpeechMs_ - startMs_));
        if (durationMs >= config.minSpeechMs) {
            SpeechEvent end;
            end.type = SpeechEventType::End;
            end.timestamp_ms = frame.timestamp_ms;
            if (speechEventQueue_) speechEventQueue_->Push(std::move(end));
            FCITX_INFO() << "[voice-input:vadworker] Utterance end, "
                         << (durationMs / 1000) << "." << (durationMs % 1000) << "s";
        } else {
            SpeechEvent cancel;
            cancel.type = SpeechEventType::Cancel;
            cancel.timestamp_ms = frame.timestamp_ms;
            if (speechEventQueue_) speechEventQueue_->Push(std::move(cancel));
            FCITX_DEBUG() << "[voice-input:vadworker] Utterance too short ("
                          << durationMs << "ms < " << config.minSpeechMs
                          << "ms), cancelled";
        }

        silero_->Reset();
        if (vadStatusCb_) {
            vadStatusCb_(false);
        }
        ResetSession();
    }
}

void VADWorker::AppendPreRoll(
    const std::array<int16_t, kWindowSize>& pcm, size_t maxPreRollSamples) {
    for (auto sample : pcm) {
        preRoll_.push_back(sample);
    }

    while (preRoll_.size() > maxPreRollSamples) {
        preRoll_.pop_front();
    }
}

void VADWorker::ResetSession() {
    state_ = State::Idle;
    preRoll_.clear();
    speechFrames_ = 0;
    silenceFrames_ = 0;
    startMs_ = 0;
    lastSpeechMs_ = 0;
}

} // namespace fcitx
