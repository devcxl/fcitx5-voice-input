#include <array>
#include <cstddef>
#include <iostream>
#include <vector>

#include "capture/audio_buffer_segments.h"
#include "capture/audio_frame_assembler.h"

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool TestUnalignedBlocksPreserveEverySample() {
    fcitx::AudioFrameAssembler assembler;
    std::vector<float> source(2048);
    for (size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<float>(i);
    }

    std::vector<float> output;
    auto emit = [&output](const std::array<float, fcitx::kWindowSize>& frame) {
        output.insert(output.end(), frame.begin(), frame.end());
    };

    assembler.Append(source.data(), 256, emit);
    assembler.Append(source.data() + 256, 480, emit);
    assembler.Append(source.data() + 736, 960, emit);
    if (!Check(output.size() == 1536, "three complete frames must be emitted")) {
        return false;
    }
    if (!Check(assembler.PendingSamples() == 160,
               "the remaining samples must be retained")) {
        return false;
    }

    assembler.Append(source.data() + 1696, 352, emit);
    if (!Check(output.size() == source.size(),
               "the final block must complete the fourth frame")) {
        return false;
    }
    for (size_t i = 0; i < source.size(); ++i) {
        if (!Check(output[i] == source[i], "audio samples must remain contiguous")) {
            return false;
        }
    }
    return Check(assembler.PendingSamples() == 0,
                 "no samples must remain after an exact number of frames");
}

bool TestCircularBufferOffsetPreservesValidOrder() {
    const std::array<float, 8> source{0, 1, 2, 3, 4, 5, 6, 7};
    std::vector<float> output;
    fcitx::ForEachCircularFloatSegment(
        source.data(), source.size() * sizeof(float), 6 * sizeof(float),
        4 * sizeof(float),
        [&output](const float* segment, size_t count) {
            output.insert(output.end(), segment, segment + count);
        });

    return Check(output == std::vector<float>({6, 7, 0, 1}),
                 "a wrapped SPA chunk must begin at its offset");
}

} // namespace

int main() {
    return TestUnalignedBlocksPreserveEverySample() &&
                   TestCircularBufferOffsetPreservesValidOrder()
               ? 0
               : 1;
}
