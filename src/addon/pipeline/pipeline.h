#pragma once

#include <memory>
#include <atomic>
#include <functional>
#include <thread>

#include "config/config.h"
#include "capture/pipewire_capture.h"
#include "vad/vad.h"
#include "asr/asr_engine.h"

namespace fcitx {

/**
 * Voice input pipeline — state machine orchestrating the full flow:
 *
 *   IDLE → RECORDING → PROCESSING_ASR → [PROCESSING_LLM] → IDLE
 *
 * Pipeline state changes are instantaneous and non-blocking on the
 * main thread. ASR runs on a separate thread; results arrive via callback.
 */
class Pipeline {
public:
    enum class State {
        IDLE,
        RECORDING,
        PROCESSING_ASR,
        PROCESSING_LLM,
    };

    using StateCallback = std::function<void(State oldState, State newState)>;
    using ResultCallback = std::function<void(const std::string& text)>;

    Pipeline();
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────
    void Init(const VoiceInputConfig& config);
    void StartRecording();
    void StopRecording();
    void Abort();

    State GetState() const { return state_.load(); }
    const char* StateName() const;

    // ── Configuration (thread-safe) ────────────────────────────────────
    void SetConfig(const VoiceInputConfig& config);
    void SetAsrEngine(std::unique_ptr<AsrEngine> engine);

    // ── Callbacks ──────────────────────────────────────────────────────
    void SetStateCallback(StateCallback cb) { stateCb_ = std::move(cb); }
    void SetResultCallback(ResultCallback cb) { resultCb_ = std::move(cb); }

private:
    // Main processing loop (runs on capture thread)
    void CaptureLoop();

    // VAD processing step
    void ProcessVAD();

    // Handle audio accumulation and dispatch to ASR
    void DispatchToAsr();

    // ASR result callback (called from ASR thread)
    void OnAsrResult(const std::string& text, bool isFinal);

    // State transition helper
    void SetState(State newState);

    // Audio buffer for current recording session
    std::vector<float> sessionAudio_;
    static constexpr size_t kSessionReserveSamples = 48000 * 10;  // ~10s at 16kHz

    // Components
    std::unique_ptr<PipeWireCapture> capture_;
    std::unique_ptr<VAD> vad_;
    std::unique_ptr<AsrEngine> asrEngine_;
    VoiceInputConfig config_;

    // State
    std::atomic<State> state_{State::IDLE};
    std::unique_ptr<std::thread> captureThread_;
    std::atomic<bool> asrCancelled_{false};

    // Callbacks
    StateCallback stateCb_;
    ResultCallback resultCb_;
};

} // namespace fcitx
