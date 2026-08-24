#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace fcitx {

// 用外部会话 generation 隔离请求，并串行化最终发布与取消。
class LLMRequestCancellation {
public:
    void Activate(uint64_t generation) {
        std::lock_guard<std::mutex> lock(publicationMutex_);
        activeGeneration_.store(generation, std::memory_order_release);
    }

    void Cancel() {
        std::lock_guard<std::mutex> lock(publicationMutex_);
        activeGeneration_.store(0, std::memory_order_release);
    }

    bool IsCancelled(uint64_t requestGeneration) const {
        return requestGeneration == 0 ||
               activeGeneration_.load(std::memory_order_acquire) !=
                   requestGeneration;
    }

    // 回调在发布门内执行，必须保持短小且不得递归 Activate()/Cancel()。
    template <typename Publish>
    bool PublishIfCurrent(uint64_t requestGeneration, Publish&& publish) const {
        std::lock_guard<std::mutex> lock(publicationMutex_);
        if (IsCancelled(requestGeneration)) return false;
        publish();
        return true;
    }

private:
    std::atomic<uint64_t> activeGeneration_{0};
    mutable std::mutex publicationMutex_;
};

} // namespace fcitx
