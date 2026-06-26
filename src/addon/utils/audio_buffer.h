#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <atomic>

/**
 * Lock-free single-producer single-consumer audio ring buffer.
 *
 * Designed for PipeWire audio callbacks where the callback (producer)
 * cannot hold locks. The consumer reads from the ASR thread side.
 *
 * Thread safety: SPSC — no mutex, only std::atomic size_t for head/tail.
 */
class AudioRingBuffer {
public:
    explicit AudioRingBuffer(size_t capacity = 65536)
        : capacity_(capacity)
        , buffer_(std::make_unique<float[]>(capacity))
        , head_(0)
        , tail_(0)
    {}

    AudioRingBuffer(const AudioRingBuffer&) = delete;
    AudioRingBuffer& operator=(const AudioRingBuffer&) = delete;

    // ── Producer (PipeWire callback) ────────────────────────────────────
    // Returns number of floats actually written.
    size_t Write(const float* data, size_t count) {
        size_t written = 0;
        while (written < count) {
            size_t available = capacity_ - Size();
            if (available == 0) break;  // buffer full, drop

            size_t to_write = count - written;
            size_t contiguous = capacity_ - (head_ % capacity_);
            if (to_write > contiguous) to_write = contiguous;
            if (to_write > available) to_write = available;

            std::memcpy(&buffer_[head_ % capacity_], data + written,
                        to_write * sizeof(float));
            head_.store(head_.load(std::memory_order_relaxed) + to_write,
                        std::memory_order_release);
            written += to_write;
        }
        return written;
    }

    // ── Consumer (ASR / VAD thread) ─────────────────────────────────────
    size_t Read(float* out, size_t count) {
        size_t read = 0;
        while (read < count) {
            size_t available = Size();
            if (available == 0) break;

            size_t to_read = count - read;
            size_t contiguous = capacity_ - (tail_ % capacity_);
            if (to_read > contiguous) to_read = contiguous;
            if (to_read > available) to_read = available;

            std::memcpy(out + read, &buffer_[tail_ % capacity_],
                        to_read * sizeof(float));
            tail_.store(tail_.load(std::memory_order_relaxed) + to_read,
                        std::memory_order_release);
            read += to_read;
        }
        return read;
    }

    size_t Size() const {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_acquire);
        return h - t;
    }

    bool Empty() const { return Size() == 0; }

    // NOTE: Clear() omitted intentionally. Clearing the buffer while the
    // PipeWire callback may be writing concurrently causes a data race.
    // Instead, drain stale audio via Read() until empty before starting
    // a new session (see Pipeline::StartListening).

    size_t Capacity() const { return capacity_; }

private:
    const size_t capacity_;
    std::unique_ptr<float[]> buffer_;
    alignas(64) std::atomic<size_t> head_ {0};
    alignas(64) std::atomic<size_t> tail_ {0};
};
