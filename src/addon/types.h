#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fcitx {

constexpr int kSampleRate = 16000;
constexpr int kWindowSize = 512;  // 32ms at 16kHz
constexpr int kFrameMs = 32;

struct AudioFrame {
    int64_t timestamp_ms = 0;
    std::array<int16_t, kWindowSize> pcm{};
};

struct Utterance {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    std::vector<int16_t> pcm;
};

struct AsrResult {
    std::string text;
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    uint64_t generation = 0;
};

} // namespace fcitx
