#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

#include "types.h"

namespace fcitx {

// 将任意长度的 float PCM 连续组装为固定大小的音频帧。
class AudioFrameAssembler {
public:
    template <typename Emit>
    void Append(const float* samples, size_t count, Emit&& emit) {
        while (count > 0) {
            const size_t copied = std::min(count, kWindowSize - pendingSamples_);
            std::copy_n(samples, copied, pending_.data() + pendingSamples_);
            samples += copied;
            count -= copied;
            pendingSamples_ += copied;

            if (pendingSamples_ == kWindowSize) {
                emit(pending_);
                pendingSamples_ = 0;
            }
        }
    }

    size_t PendingSamples() const { return pendingSamples_; }

    void Reset() { pendingSamples_ = 0; }

private:
    std::array<float, kWindowSize> pending_{};
    size_t pendingSamples_ = 0;
};

} // namespace fcitx
