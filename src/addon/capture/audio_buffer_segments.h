#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace fcitx {

// 按 SPA chunk 的环形字节偏移遍历连续的 float PCM 片段。
template <typename Emit>
void ForEachCircularFloatSegment(const float* data, uint32_t maxSize,
                                 uint32_t offset, uint32_t size, Emit&& emit) {
    if (!data || maxSize < sizeof(float) ||
        maxSize % sizeof(float) != 0 || offset % sizeof(float) != 0 ||
        size % sizeof(float) != 0) {
        return;
    }

    const uint32_t validSize = std::min(size, maxSize);
    const size_t capacity = maxSize / sizeof(float);
    const size_t start = ((offset % maxSize) / sizeof(float)) % capacity;
    const size_t count = validSize / sizeof(float);
    const size_t first = std::min(count, capacity - start);

    if (first > 0) {
        emit(data + start, first);
    }
    if (count > first) {
        emit(data, count - first);
    }
}

} // namespace fcitx
