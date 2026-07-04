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

} // namespace

VADWorker::VADWorker() = default;

VADWorker::~VADWorker() {
    Stop();
}

void VADWorker::SetConfig(const Config& config) {
    config_ = config;

    FCITX_INFO() << "[voice-input:vadworker] Config:"
                 << " speechThresh=" << config_.speechThreshold
                 << " silenceThresh=" << config_.silenceThreshold
                 << " startFrames=" << config_.startFrames
                 << " preRollMs=" << config_.preRollMs
                 << " endSilenceMs=" << config_.endSilenceMs
                 << " minSpeechMs=" << config_.minSpeechMs
                 << " maxSpeechMs=" << config_.maxSpeechMs;
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

        float prob = silero_->Predict(frame.pcm.data(), frame.pcm.size());
        if (prob < 0.0f) {
            // Inference failed
            continue;
        }

        ProcessFrame(frame, prob);
    }
}

void VADWorker::ProcessFrame(const AudioFrame& frame, float probability) {
    bool speechStart = probability >= config_.speechThreshold;
    bool speechKeep = probability >= config_.silenceThreshold;

    AppendPreRoll(frame.pcm);

    if (state_ == State::Idle) {
        if (speechStart) {
            speechFrames_++;
            if (speechFrames_ >= config_.startFrames) {
                state_ = State::Speaking;
                startMs_ = frame.timestamp_ms - config_.preRollMs;

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
        config_.endSilenceMs / kFrameMs;
    bool silenceEnd = silenceFrames_ >= endSilenceFrames;

    int maxDurationMs = config_.maxSpeechMs;
    bool tooLong =
        (lastSpeechMs_ - startMs_) >= maxDurationMs;

    if (silenceEnd || tooLong) {
        int durationMs = static_cast<int>((lastSpeechMs_ - startMs_));
        if (durationMs >= config_.minSpeechMs) {
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
                          << durationMs << "ms < " << config_.minSpeechMs
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
    const std::array<int16_t, kWindowSize>& pcm) {
    for (auto sample : pcm) {
        preRoll_.push_back(sample);
    }

    size_t maxPreRollSamples =
        static_cast<size_t>(kSampleRate) * config_.preRollMs / 1000;
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
