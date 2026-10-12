#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include "types.h"
#include "utils/thread_safe_queue.h"

namespace fcitx {

class AudioCapture {
public:
    virtual ~AudioCapture() = default;

    virtual bool Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
    virtual const char* Name() const = 0;
    // Main thread: mute delivery without closing the manager-visible stream.
    // The lock synchronizes with publication, not with a blocking audio read.
    void SetDeliveryEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(deliveryMutex_);
        const auto epoch = deliveryEpoch_.load(std::memory_order_relaxed);
        if (DeliveryEnabled(epoch) != enabled) {
            deliveryEpoch_.store(epoch + 1, std::memory_order_release);
        }
    }
    virtual void SetFrameQueue(ThreadSafeQueue<AudioFrame>* queue) {
        frameQueue_ = queue;
    }

protected:
    uint64_t DeliveryEpoch() const {
        return deliveryEpoch_.load(std::memory_order_acquire);
    }
    static bool DeliveryEnabled(uint64_t epoch) { return (epoch & 1) != 0; }
    void DeliverFrame(AudioFrame frame, uint64_t epoch) {
        std::lock_guard<std::mutex> lock(deliveryMutex_);
        if (DeliveryEnabled(epoch) && epoch == DeliveryEpoch() && frameQueue_) {
            frameQueue_->Push(std::move(frame));
        }
    }
    ThreadSafeQueue<AudioFrame>* frameQueue_ = nullptr;

private:
    // Odd = delivering, even = discarding. An epoch, rather than a bool, also
    // rejects a read spanning a complete pause/resume cycle (issue #54).
    std::atomic<uint64_t> deliveryEpoch_{1};
    std::mutex deliveryMutex_;
};

} // namespace fcitx
