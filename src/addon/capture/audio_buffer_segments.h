#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace fcitx {

// 按 SPA chunk 的环形字节偏移与 stride 遍历 float PCM 片段。
template <typename Emit>
void ForEachCircularFloatSegment(const float* data, uint32_t maxSize,
                                 uint32_t offset, uint32_t size, int32_t stride,
                                 Emit&& emit) {
    constexpr int32_t kPackedFloatStride = sizeof(float);
    if (!data || maxSize < sizeof(float) ||
        (stride != 0 && stride != kPackedFloatStride)) {
        return;
    }

    const uint32_t validSize = std::min(size, maxSize);
    const uint32_t start = offset % maxSize;
    if (maxSize % sizeof(float) != 0 || start % sizeof(float) != 0 ||
        validSize % sizeof(float) != 0) {
        return;
    }

    const size_t capacity = maxSize / sizeof(float);
    const size_t sampleStart = start / sizeof(float);
    const size_t count = validSize / sizeof(float);
    const size_t first = std::min(count, capacity - sampleStart);

    if (first > 0) {
        emit(data + sampleStart, first);
    }
    if (count > first) {
        emit(data, count - first);
    }
}

} // namespace fcitx
