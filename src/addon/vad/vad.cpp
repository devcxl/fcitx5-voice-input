#include "vad.h"

#include <cmath>
#include <algorithm>

namespace fcitx {

VAD::VAD()
    : config_(Config{})
{
}

VAD::VAD(const Config& config)
    : config_(config)
{
}

void VAD::SetConfig(const Config& config) {
    config_ = config;
    Reset();
}

void VAD::Reset() {
    speechActive_ = false;
    silenceFrameCount_ = 0;
    initialized_ = false;
}

float VAD::ComputeEnergy(const float* frame, size_t len) const {
    float sum = 0.0f;
    for (size_t i = 0; i < len; ++i) {
        sum += frame[i] * frame[i];
    }
    return std::sqrt(sum / static_cast<float>(len));
}

bool VAD::Process(const float* pcm, size_t frames) {
    // Process in frameSize chunks
    size_t offset = 0;
    while (offset + config_.frameSize <= frames) {
        float energy = ComputeEnergy(pcm + offset, config_.frameSize);

        // Simple energy threshold with hysteresis
        if (speechActive_) {
            if (energy < config_.threshold * 0.5f) {
                // Below silence threshold
                silenceFrameCount_++;
                if (silenceFrameCount_ >= config_.silenceFrames) {
                    speechActive_ = false;
                }
            } else {
                // Still within active speech
                silenceFrameCount_ = 0;
            }
        } else {
            if (energy > config_.threshold) {
                // Speech detected
                speechActive_ = true;
                silenceFrameCount_ = 0;
            } else {
                silenceFrameCount_++;
            }
        }

        offset += config_.frameSize;
    }

    return speechActive_;
}

bool VAD::IsSilenceTimeout() const {
    return !speechActive_ && silenceFrameCount_ >= config_.silenceFrames;
}

} // namespace fcitx
