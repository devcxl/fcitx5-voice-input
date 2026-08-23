#pragma once

#include <atomic>
#include <cstdint>

namespace fcitx {

// 用取消代次隔离不同会话：Cancel() 只中断已开始的请求，后续请求自动使用新代次。
class LLMRequestCancellation {
public:
    uint64_t BeginRequest() const {
        return generation_.load(std::memory_order_acquire);
    }

    void Cancel() {
        generation_.fetch_add(1, std::memory_order_acq_rel);
    }

    bool IsCancelled(uint64_t requestGeneration) const {
        return generation_.load(std::memory_order_acquire) != requestGeneration;
    }

private:
    std::atomic<uint64_t> generation_{0};
};

} // namespace fcitx
