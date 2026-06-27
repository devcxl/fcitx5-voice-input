#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace fcitx {

class SileroVad {
public:
    explicit SileroVad(const std::string& modelPath);
    ~SileroVad();

    SileroVad(const SileroVad&) = delete;
    SileroVad& operator=(const SileroVad&) = delete;

    // Returns speech probability in [0.0, 1.0] given 512 int16_t samples.
    // Returns -1.0f on inference failure.
    float Predict(const int16_t* pcm, size_t samples);

    void Reset();

    bool IsReady() const { return ready_; }

    static constexpr int WindowSize() { return kWindowSamples; }

private:
    static constexpr int kSampleRate = 16000;
    static constexpr size_t kWindowSamples = 512;
    static constexpr size_t kContextSamples = 64;
    static constexpr size_t kEffectiveSamples = kWindowSamples + kContextSamples;
    static constexpr size_t kStateSize = 2 * 1 * 128;

    bool Init(const std::string& modelPath);

    bool ready_ = false;

    // ONNX Runtime state (opaque via forward declaration would be cleaner,
    // but keeping it simple with conditional compilation)
    void* env_ = nullptr;
    void* session_ = nullptr;
    void* memoryInfo_ = nullptr;
    void* options_ = nullptr;

    std::array<float, kEffectiveSamples> input_{};
    std::array<float, kContextSamples> context_{};
    std::array<float, kStateSize> state_{};
    std::array<int64_t, 1> sr_{kSampleRate};

    std::string modelPath_;
};

} // namespace fcitx
