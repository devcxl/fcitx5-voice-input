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
    vadConfig.threshold = config_.vadThreshold;
    vadConfig.silenceFrames = config_.silenceThresholdMs / 20;
    vad_->SetConfig(vadConfig);

    if (!capture_->Start()) {
        std::cerr << "[voice-input] Failed to start PipeWire capture" << std::endl;
    }

    SetState(State::IDLE);
}

void Pipeline::StartRecording() {
    if (state_.load() != State::IDLE) return;

    // Clear previous session data
    sessionAudio_.clear();
    sessionAudio_.reserve(kSessionReserveSamples);

    // Drain any stale audio from the ring buffer instead of Clear().
    // Clear() with the PipeWire callback writing concurrently is a
    // data race — Read() is safe to call concurrently with Write().
    float discard[320];
    for (int i = 0; i < 1000; ++i) {
        if (capture_->RingBuffer()->Read(discard, 320) == 0) break;
    }

    vad_->Reset();

    SetState(State::RECORDING);

    // Start capture loop thread
    captureThread_ = std::make_unique<std::thread>(&Pipeline::CaptureLoop, this);
}

void Pipeline::StopRecording() {
    State expected = State::RECORDING;
    if (!state_.compare_exchange_strong(expected, State::PROCESSING_ASR)) {
        return;  // not recording
    }

    // Wait for capture thread to finish
    if (captureThread_ && captureThread_->joinable()) {
        captureThread_->join();
        captureThread_.reset();
    }

    // Dispatch accumulated audio to the ASR engine (non-blocking).
    // The engine processes on its own inference thread and fires
    // the result callback asynchronously.
    DispatchToAsr();
}

void Pipeline::Abort() {
    asrCancelled_ = true;

    if (captureThread_ && captureThread_->joinable()) {
        captureThread_->join();
        captureThread_.reset();
    }

    // Signal the ASR engine to stop (non-blocking — just pushes EOF)
    if (asrEngine_) {
        asrEngine_->Stop();
    }

    state_.store(State::IDLE);
}

void Pipeline::SetConfig(const VoiceInputConfig& config) {
    config_ = config;
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
    // Read from ring buffer in a polling loop
    constexpr size_t chunkFrames = 320;  // 20ms at 16kHz
    float chunk[chunkFrames];

    while (state_.load() == State::RECORDING) {
        size_t read = capture_->RingBuffer()->Read(chunk, chunkFrames);
        if (read == 0) {
            // No data available, yield
            std::this_thread::sleep_for(5ms);
            continue;
        }

        // Process VAD on the captured chunk
        vad_->Process(chunk, read);

        // Accumulate speech audio
        if (vad_->IsSpeechActive()) {
            sessionAudio_.insert(sessionAudio_.end(), chunk, chunk + read);
        } else if (!sessionAudio_.empty()) {
            // Keep a short tail after speech ends (for VAD margin)
            sessionAudio_.insert(sessionAudio_.end(), chunk, chunk + read);
        }
    }
}

void Pipeline::ProcessVAD() {
    // VAD processing is integrated into CaptureLoop
}

void Pipeline::DispatchToAsr() {
    if (!asrEngine_) {
        std::cerr << "[voice-input] No ASR engine configured" << std::endl;
        SetState(State::IDLE);
        return;
    }

    if (sessionAudio_.empty()) {
        std::cerr << "[voice-input] No audio to process" << std::endl;
        SetState(State::IDLE);
        return;
    }

    // Start ASR session and feed all accumulated audio.
    // The engine starts its own inference thread; Start/FeedAudio/Stop
    // are all non-blocking (the thread processes audio asynchronously).
    asrEngine_->Start();
    asrEngine_->FeedAudio(sessionAudio_.data(), sessionAudio_.size());
    sessionAudio_.clear();
    asrEngine_->Stop();  // non-blocking — signals EOF, returns immediately
}

void Pipeline::OnAsrResult(const std::string& text, bool isFinal) {
    // Called from ASR inference thread — check if pipeline is still alive
    // (could have been cancelled by Abort/cleanup)
    if (asrCancelled_) return;

    if (isFinal && !text.empty() && resultCb_) {
        resultCb_(text);
        SetState(State::IDLE);
    }
}

} // namespace fcitx
