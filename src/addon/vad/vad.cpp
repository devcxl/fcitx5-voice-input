#include "vad.h"

#include <cmath>
#include <algorithm>
#include <fcitx-utils/log.h>

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
    FCITX_INFO() << "[voice-input:vad] Config: threshold=" << config_.threshold
                 << " silenceFrames=" << config_.silenceFrames
                 << " frameSize=" << config_.frameSize;
    Reset();
}

void VAD::Reset() {
    speechActive_ = false;
    silenceFrameCount_ = 0;
    initialized_ = false;
    frameCount_ = 0;
    FCITX_DEBUG() << "[voice-input:vad] Reset (was active=" << speechActive_
                  << " silenceFrames=" << silenceFrameCount_ << ")";
}

float VAD::ComputeEnergy(const float* frame, size_t len) const {
    float sum = 0.0f;
    for (size_t i = 0; i < len; ++i) {
        sum += frame[i] * frame[i];
    }
    float energy = std::sqrt(sum / static_cast<float>(len));
    return energy;
}

bool VAD::Process(const float* pcm, size_t frames) {
    constexpr bool kLogEnergy = true;
    size_t offset = 0;
    while (offset + config_.frameSize <= frames) {
        frameCount_++;
        float energy = ComputeEnergy(pcm + offset, config_.frameSize);

        // Simple energy threshold with hysteresis
        if (speechActive_) {
            if (energy < config_.threshold * 0.5f) {
                // Below silence threshold
                silenceFrameCount_++;
                if (silenceFrameCount_ >= config_.silenceFrames) {
                    FCITX_INFO() << "[voice-input:vad] Silence timeout: energy=" << energy
                                 << " threshold=" << (config_.threshold * 0.5f)
                                 << " silenceFrameCount=" << silenceFrameCount_
                                 << " totalFrames=" << frameCount_;
                    speechActive_ = false;
                } else if (kLogEnergy && silenceFrameCount_ > config_.silenceFrames / 2) {
                    FCITX_DEBUG() << "[voice-input:vad] Silence accumulating: energy=" << energy
                                  << " silenceFrameCount=" << silenceFrameCount_
                                  << "/" << config_.silenceFrames;
                }
            } else {
                // Still within active speech
                if (silenceFrameCount_ > 0) {
                    FCITX_DEBUG() << "[voice-input:vad] Speech resumed: energy=" << energy
                                  << " (was accumulating silence for " << silenceFrameCount_ << " frames)";
                }
                silenceFrameCount_ = 0;
            }
        } else {
            if (energy > config_.threshold) {
                // Speech detected
                FCITX_INFO() << "[voice-input:vad] Speech ONSET detected: energy=" << energy
                             << " threshold=" << config_.threshold
                             << " frame=" << frameCount_;
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
