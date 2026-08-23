#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
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
        4 * sizeof(float), sizeof(float),
        [&output](const float* segment, size_t count) {
            output.insert(output.end(), segment, segment + count);
        });

    return Check(output == std::vector<float>({6, 7, 0, 1}),
                 "a wrapped SPA chunk must begin at its offset");
}

bool TestUnexpectedStrideIsRejected() {
    const std::array<float, 8> source{0, -1, 2, -1, 4, -1, 6, -1};
    size_t emitted = 0;
    constexpr std::array<int32_t, 7> invalidStrides{
        std::numeric_limits<int32_t>::min(), -4, 1, 2, 3, 5, 8};
    for (int32_t stride : invalidStrides) {
        fcitx::ForEachCircularFloatSegment(
            source.data(), source.size() * sizeof(float), 0,
            source.size() * sizeof(float), stride,
            [&emitted](const float*, size_t count) { emitted += count; });
    }

    return Check(emitted == 0, "a non-mono-F32 stride must be rejected");
}

bool TestZeroStrideUsesPackedSamples() {
    const std::array<float, 3> source{1, 2, 3};
    std::vector<float> output;
    fcitx::ForEachCircularFloatSegment(
        source.data(), source.size() * sizeof(float), 0,
        source.size() * sizeof(float), 0,
        [&output](const float* segment, size_t count) {
            output.insert(output.end(), segment, segment + count);
        });

    return Check(output == std::vector<float>({1, 2, 3}),
                 "zero stride must retain packed-audio compatibility");
}

bool TestChunkSizeIsClampedToCapacity() {
    const std::array<float, 4> source{0, 1, 2, 3};
    std::vector<float> output;
    fcitx::ForEachCircularFloatSegment(
        source.data(), source.size() * sizeof(float), 6 * sizeof(float),
        std::numeric_limits<uint32_t>::max(), sizeof(float),
        [&output](const float* segment, size_t count) {
            output.insert(output.end(), segment, segment + count);
        });

    return Check(output == std::vector<float>({2, 3, 0, 1}),
                 "SPA chunk size must be clamped before traversal");
}

bool TestUnalignedChunkIsRejected() {
    const std::array<float, 4> source{0, 1, 2, 3};
    size_t emitted = 0;
    auto count = [&emitted](const float*, size_t size) { emitted += size; };
    fcitx::ForEachCircularFloatSegment(source.data(), sizeof(source), 1,
                                       sizeof(source), sizeof(float), count);
    fcitx::ForEachCircularFloatSegment(source.data(), sizeof(source), 0,
                                       sizeof(source) - 1, sizeof(float), count);

    return Check(emitted == 0, "unaligned float chunks must be rejected");
}

} // namespace

int main() {
    return TestUnalignedBlocksPreserveEverySample() &&
                   TestCircularBufferOffsetPreservesValidOrder() &&
                   TestUnexpectedStrideIsRejected() &&
                   TestZeroStrideUsesPackedSamples() &&
                   TestChunkSizeIsClampedToCapacity() &&
                   TestUnalignedChunkIsRejected()
               ? 0
               : 1;
}
