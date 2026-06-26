#include "pipeline.h"

#include <cstring>
#include <iostream>
#include <chrono>
#include <thread>
#include <memory>

using namespace std::chrono_literals;

namespace fcitx {

Pipeline::Pipeline()
    : capture_(std::make_unique<PipeWireCapture>())
    , vad_(std::make_unique<VAD>())
{
}

Pipeline::~Pipeline() {
    Abort();
    capture_->Stop();
}

void Pipeline::Init(const VoiceInputConfig& config) {
    config_ = config;

    VAD::Config vadConfig;
    vadConfig.threshold = static_cast<float>(config_.vadThreshold.value()) / 100.0f;
    vadConfig.silenceFrames = config_.silenceThresholdMs.value() / 20;
    vad_->SetConfig(vadConfig);
}

void Pipeline::StartRecording() {
    if (state_.load() != State::IDLE) return;

    // Reset session
    sessionAudio_.clear();
    sessionAudio_.reserve(kSessionReserveSamples);
    asrCancelled_.store(false);

    // Transition to RECORDING
    SetState(State::RECORDING);

    // Start capture thread
    captureThread_ = std::make_unique<std::thread>([this]() {
        capture_->Start();

        // Allocate a fixed VAD buffer for real-time processing
        VAD vad(*vad_);
        int silenceFrames = 0;

        while (true) {
            auto buffer = capture_->Read();
            if (!buffer || buffer->empty()) {
                if (state_.load() != State::RECORDING) break;
                std::this_thread::sleep_for(5ms);
                continue;
            }

            // Process VAD
            bool hasVoice = vad.Process(*buffer);

            // Accumulate audio (always, for ASR)
            sessionAudio_.insert(sessionAudio_.end(),
                                 buffer->begin(), buffer->end());

            // State transitions based on VAD
            if (hasVoice) {
                // Reset silence counter
                silenceFrames = 0;
            } else {
                silenceFrames += buffer->size() / vad_.GetConfig().frameSize;
                if (silenceFrames >= vad_.GetConfig().silenceFrames) {
                    // Silence detected → stop recording
                    capture_->Stop();
                    break;
                }
            }
        }

        // Stop recording and dispatch to ASR
        capture_->Stop();
        if (!asrCancelled_.load()) {
            SetState(State::PROCESSING_ASR);
            DispatchToAsr();
        }
    });
}

void Pipeline::StopRecording() {
    auto expected = State::RECORDING;
    if (state_.compare_exchange_strong(expected, State::IDLE)) {
        asrCancelled_.store(true);
        capture_->Stop();
        if (captureThread_ && captureThread_->joinable()) {
            captureThread_->join();
        }
        SetState(State::IDLE);
    }
}

void Pipeline::Abort() {
    asrCancelled_.store(true);
    capture_->Stop();
    if (captureThread_ && captureThread_->joinable()) {
        captureThread_->join();
    }
    SetState(State::IDLE);
}

void Pipeline::SetConfig(const VoiceInputConfig& config) {
    config_ = config;

    VAD::Config vadConfig;
    vadConfig.threshold = static_cast<float>(config_.vadThreshold.value()) / 100.0f;
    vadConfig.silenceFrames = config_.silenceThresholdMs.value() / 20;
    vad_->SetConfig(vadConfig);
}

void Pipeline::SetAsrEngine(std::unique_ptr<AsrEngine> engine) {
    asrEngine_ = std::move(engine);
}

const char* Pipeline::StateName() const {
    switch (state_.load()) {
        case State::IDLE: return "IDLE";
        case State::RECORDING: return "RECORDING";
        case State::PROCESSING_ASR: return "PROCESSING_ASR";
        case State::PROCESSING_LLM: return "PROCESSING_LLM";
    }
    return "UNKNOWN";
}

void Pipeline::DispatchToAsr() {
    if (!asrEngine_) {
        FCITX_WARN() << "[voice-input] No ASR engine set";
        SetState(State::IDLE);
        return;
    }

    auto audio = std::make_shared<std::vector<float>>(std::move(sessionAudio_));
    sessionAudio_.clear();

    asrEngine_->Transcribe(audio, 16000,
        [this](const std::string& text, bool isFinal) {
            if (isFinal) {
                if (!text.empty() && resultCb_) {
                    resultCb_(text);
                }
                SetState(State::IDLE);
            } else {
                if (!text.empty() && resultCb_) {
                    resultCb_(text);
                }
            }
        });
}

void Pipeline::SetState(State newState) {
    State old = state_.exchange(newState);
    if (old != newState && stateCb_) {
        stateCb_(old, newState);
    }
}

} // namespace fcitx
