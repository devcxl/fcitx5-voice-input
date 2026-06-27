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

void VADWorker::SetUtteranceQueue(ThreadSafeQueue<Utterance>* queue) {
    utteranceQueue_ = queue;
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
                // Speech onset
                state_ = State::Speaking;
                startMs_ = frame.timestamp_ms - config_.preRollMs;

                currentAudio_.clear();
                currentAudio_.insert(currentAudio_.end(),
                                    preRoll_.begin(), preRoll_.end());

                silenceFrames_ = 0;
                lastSpeechMs_ = frame.timestamp_ms;
                speechFrames_ = 0;

                FCITX_INFO() << "[voice-input:vadworker] Speech onset"
                             << " startMs=" << startMs_
                             << " preRollSamples=" << preRoll_.size();
            }
        } else {
            speechFrames_ = 0;
        }
        return;
    }

    // State::Speaking
    currentAudio_.insert(currentAudio_.end(),
                         frame.pcm.begin(), frame.pcm.end());

    if (speechKeep) {
        silenceFrames_ = 0;
        lastSpeechMs_ = frame.timestamp_ms;
    } else {
        silenceFrames_++;
    }

    int endSilenceFrames =
        config_.endSilenceMs / kFrameMs;
    bool silenceEnd = silenceFrames_ >= endSilenceFrames;

    int maxSamples = kSampleRate * config_.maxSpeechMs / 1000;
    bool tooLong = currentAudio_.size() >= static_cast<size_t>(maxSamples);

    if (silenceEnd || tooLong) {
        FlushUtterance(frame.timestamp_ms);
    }
}

void VADWorker::FlushUtterance(int64_t endMs) {
    int durationMs =
        static_cast<int>((lastSpeechMs_ - startMs_));
    int minSpeechSamples =
        kSampleRate * config_.minSpeechMs / 1000;

    if (static_cast<int>(currentAudio_.size()) >= minSpeechSamples) {
        Utterance u;
        u.start_ms = startMs_;
        u.end_ms = lastSpeechMs_;
        u.pcm = std::move(currentAudio_);

        float durSec = static_cast<float>(durationMs) / 1000.0f;
        FCITX_INFO() << "[voice-input:vadworker] Utterance: "
                     << durSec << "s, "
                     << u.pcm.size() << " samples";

        if (utteranceQueue_) {
            utteranceQueue_->Push(std::move(u));
        }
    } else {
        FCITX_DEBUG() << "[voice-input:vadworker] Utterance too short ("
                      << durationMs << "ms < " << config_.minSpeechMs
                      << "ms), discarded";
    }

    silero_->Reset();
    ResetSession();
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
    currentAudio_.clear();
    speechFrames_ = 0;
    silenceFrames_ = 0;
    startMs_ = 0;
    lastSpeechMs_ = 0;
}

} // namespace fcitx
