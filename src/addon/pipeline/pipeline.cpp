#include "pipeline.h"

#include <fcitx-utils/log.h>
#include <cstring>
#include <cmath>
#include <iostream>
#include <chrono>
#include <thread>
#include <memory>

#include "capture/pipewire_capture.h"
#include "capture/pulse_audio_capture.h"

using namespace std::chrono_literals;

namespace fcitx {

Pipeline::Pipeline()
    : vad_(std::make_unique<VAD>())
{
}

Pipeline::~Pipeline() {
    Abort();
    if (capture_) {
        capture_->Stop();
    }
}

void Pipeline::Init(const VoiceInputConfig& config) {
    config_ = config;

    VAD::Config vadConfig;
    vadConfig.threshold = static_cast<float>(config_.vadThreshold.value()) / 100.0f;
    vadConfig.silenceFrames = config_.silenceThresholdMs.value() / 20;
    vad_->SetConfig(vadConfig);

    FCITX_INFO() << "[voice-input] Init: vadThreshold=" << config_.vadThreshold.value()
                 << "% silenceThresholdMs=" << config_.silenceThresholdMs.value()
                 << " silenceFrames=" << vadConfig.silenceFrames;

    SetState(State::IDLE);
}

void Pipeline::StartListening() {
    if (state_.load() != State::IDLE) {
        FCITX_DEBUG() << "[voice-input] StartListening ignored: state=" << StateName();
        return;
    }

    FCITX_INFO() << "[voice-input] StartListening";

    if (!StartCapture()) {
        return;
    }

    asrCancelled_ = false;

    sessionAudio_.clear();
    sessionAudio_.reserve(kSessionReserveSamples);

    // Drain any stale audio from the ring buffer instead of Clear().
    float discard[320];
    int drained = 0;
    for (int i = 0; i < 1000; ++i) {
        if (!capture_ || capture_->RingBuffer()->Read(discard, 320) == 0) break;
        drained++;
    }
    if (drained > 0) {
        FCITX_INFO() << "[voice-input] Drained " << (drained * 320) << " stale samples from ring buffer";
    }

    vad_->Reset();

    SetState(State::LISTENING);

    captureThread_ = std::make_unique<std::thread>(&Pipeline::CaptureLoop, this);
}

void Pipeline::StopListening() {
    if (state_.load() == State::IDLE) return;

    FCITX_INFO() << "[voice-input] Pipeline::StopListening";

    asrCancelled_ = true;
    SetState(State::IDLE);

    if (captureThread_ && captureThread_->joinable()) {
        captureThread_->join();
        captureThread_.reset();
    }
    if (capture_) {
        capture_->Stop();
    }
}

void Pipeline::Abort() {
    asrCancelled_ = true;

    state_.store(State::IDLE);

    if (captureThread_ && captureThread_->joinable()) {
        captureThread_->join();
        captureThread_.reset();
    }
    if (capture_) {
        capture_->Stop();
    }

    if (asrEngine_) {
        asrEngine_->Stop();
    }
}

void Pipeline::SetConfig(const VoiceInputConfig& config) {
    config_ = config;
}

bool Pipeline::StartCapture() {
    if (capture_ && capture_->IsRunning()) {
        return true;
    }

    if (!capture_) {
        capture_ = std::make_unique<PulseAudioCapture>();
    }

    if (capture_->Start()) {
        FCITX_INFO() << "[voice-input] Using audio capture backend: " << capture_->Name();
        return true;
    }

    if (std::string(capture_->Name()) != "pipewire") {
        FCITX_WARN() << "[voice-input] Audio capture backend " << capture_->Name()
                     << " failed, falling back to pipewire";
        capture_ = std::make_unique<PipeWireCapture>();
        if (capture_->Start()) {
            FCITX_INFO() << "[voice-input] Using audio capture backend: " << capture_->Name();
            return true;
        }
    }

    FCITX_ERROR() << "[voice-input] Failed to start any audio capture backend";
    return false;
}

void Pipeline::SetAsrEngine(std::unique_ptr<AsrEngine> engine) {
    asrEngine_ = std::move(engine);
    if (asrEngine_) {
        asrEngine_->SetResultCallback(
            [this](const std::string& text, bool isFinal) {
                OnAsrResult(text, isFinal);
            });
    }
}

const char* Pipeline::StateName() const {
    switch (state_.load()) {
        case State::IDLE:            return "IDLE";
        case State::LISTENING:       return "LISTENING";
        case State::RECORDING:       return "RECORDING";
        case State::PROCESSING_ASR:  return "PROCESSING_ASR";
        case State::PROCESSING_LLM:  return "PROCESSING_LLM";
    }
    return "UNKNOWN";
}

void Pipeline::SetState(State newState) {
    State oldState = state_.exchange(newState);
    if (oldState != newState && stateCb_) {
        stateCb_(oldState, newState);
    }
}

void Pipeline::CaptureLoop() {
    constexpr size_t chunkFrames = 320;  // 20ms at 16kHz
    float chunk[chunkFrames];
    int emptyReadCount = 0;

    while (state_.load() != State::IDLE) {
        size_t read = capture_ ? capture_->RingBuffer()->Read(chunk, chunkFrames) : 0;
        if (read == 0) {
            ++emptyReadCount;
            if (emptyReadCount % 400 == 0) {
                FCITX_WARN() << "[voice-input] No audio samples read from PipeWire ring buffer for ~2s"
                             << " (backend=" << (capture_ ? capture_->Name() : "none")
                             << " running=" << (capture_ && capture_->IsRunning()) << ")";
            }
            std::this_thread::sleep_for(5ms);
            continue;
        }
        emptyReadCount = 0;

        State curState = state_.load();

        if (curState == State::LISTENING) {
            vad_->Process(chunk, read);
            if (vad_->IsSpeechActive()) {
                float energy = 0.0f;
                for (size_t i = 0; i < read; ++i) energy += std::abs(chunk[i]);
                FCITX_INFO() << "[voice-input] VAD speech onset, energy="
                             << (energy / read);
                sessionAudio_.clear();
                sessionAudio_.insert(sessionAudio_.end(), chunk, chunk + read);
                SetState(State::RECORDING);
            }
        } else if (curState == State::RECORDING) {
            vad_->Process(chunk, read);
            sessionAudio_.insert(sessionAudio_.end(), chunk, chunk + read);
            if (!vad_->IsSpeechActive() && vad_->IsSilenceTimeout()) {
                float durSec = sessionAudio_.size() / 16000.0f;
                FCITX_INFO() << "[voice-input] VAD silence timeout, audio="
                             << sessionAudio_.size() << " frames ("
                             << durSec << "s), submitting";
                SetState(State::PROCESSING_ASR);
                DispatchToAsr();
            } else if (sessionAudio_.size() >= kMaxSessionSamples) {
                FCITX_INFO() << "[voice-input] Max recording duration reached, audio="
                             << sessionAudio_.size() << " frames, submitting";
                SetState(State::PROCESSING_ASR);
                DispatchToAsr();
            }
        }
        // PROCESSING_ASR / PROCESSING_LLM: discard audio, wait for state change
        if (curState == State::PROCESSING_ASR || curState == State::PROCESSING_LLM) {
            static int discardCount = 0;
            discardCount++;
            if (discardCount % 100 == 0) {
                FCITX_DEBUG() << "[voice-input] Discarding " << (discardCount * read)
                              << " samples during " << StateName();
            }
        }
    }
    FCITX_INFO() << "[voice-input] CaptureLoop exited (state=" << StateName() << ")";
}

void Pipeline::DispatchToAsr() {
    if (!asrEngine_) {
        FCITX_ERROR() << "[voice-input] No ASR engine configured";
        SetState(State::LISTENING);
        return;
    }

    if (sessionAudio_.empty()) {
        FCITX_WARN() << "[voice-input] No audio to process";
        SetState(State::LISTENING);
        return;
    }

    float durSec = sessionAudio_.size() / 16000.0f;
    FCITX_INFO() << "[voice-input] Dispatching " << sessionAudio_.size()
                 << " frames (" << durSec << "s) to ASR engine: "
                 << (asrEngine_ ? asrEngine_->Name() : "none");

    asrEngine_->Start();
    asrEngine_->FeedAudio(sessionAudio_.data(), sessionAudio_.size());
    sessionAudio_.clear();
    asrEngine_->Stop();
}

void Pipeline::OnAsrResult(const std::string& text, bool isFinal) {
    if (asrCancelled_) return;

    if (isFinal) {
        if (!text.empty()) {
            FCITX_INFO() << "[voice-input] ASR result: \"" << text << "\"";
        } else {
            FCITX_INFO() << "[voice-input] ASR result: (empty)";
        }
        if (!text.empty() && resultCb_) {
            resultCb_(text);
        }
        SetState(State::LISTENING);
    }
}

} // namespace fcitx
