#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace fcitx {

/**
 * Voice Activity Detector (VAD).
 *
 * Designed for simple VAD on captured audio.
 * Operates on data read from the ring buffer (NOT inside PipeWire callback).
 *
 * Uses energy-based VAD as baseline. Future: replace with webrtcvad or
 * sherpa-onnx built-in VAD.
 */
class VAD {
public:
    struct Config {
        float threshold = 0.5f;        // Energy threshold (0.0–1.0)
        int sampleRate = 16000;        // Expected sample rate (Hz)
        int frameSize = 320;           // Samples per VAD frame (20ms at 16kHz)
        int silenceFrames = 25;        // 25 frames of silence ≈ 500ms
    };

    VAD();
    explicit VAD(const Config& config);

    // Set config (thread-safe, called from main thread before start)
    void SetConfig(const Config& config);
    const Config& GetConfig() const { return config_; }

    // Process a chunk of PCM data. Returns true if speech is detected.
    // Called from capture thread (outside PipeWire callback).
    bool Process(const float* pcm, size_t frames);

    // Reset VAD state (call on new recording start).
    void Reset();

    // Returns true if silence has persisted beyond the threshold.
    bool IsSilenceTimeout() const;

    // Current speech activity state.
    bool IsSpeechActive() const { return speechActive_; }

private:
    // Compute RMS energy of a frame
    float ComputeEnergy(const float* frame, size_t len) const;

    Config config_;
    bool speechActive_ = false;
    int silenceFrameCount_ = 0;
    bool initialized_ = false;
};

} // namespace fcitx
