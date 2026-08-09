#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>

/**
 * Thread-safe multi-producer multi-consumer queue.
 *
 * Used for passing audio chunks from capture thread to ASR thread,
 * and ASR results back to the main Fcitx5 event loop.
 *
 * maxSize > 0 时队列有容量上限：Push 超限丢弃最旧元素，避免消费者
 * 异常/缓慢时内存无界增长。
 */
template<typename T>
class ThreadSafeQueue {
public:
    explicit ThreadSafeQueue(size_t maxSize = 0) : maxSize_(maxSize) {}

    // Non-copyable
    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

    void Push(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (maxSize_ > 0 && queue_.size() >= maxSize_) {
                queue_.pop();  // 丢弃最旧元素（背压：新数据优先）
            }
            queue_.push(std::move(value));
        }
        cv_.notify_one();
    }

    bool TryPop(T& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        value = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    T Pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || stopped_; });
        if (stopped_ && queue_.empty()) return T{};
        T value = std::move(queue_.front());
        queue_.pop();
        return value;
    }

    std::optional<T> TryPop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        T value = std::move(queue_.front());
        queue_.pop();
        return value;
    }

    bool Empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) queue_.pop();
    }

private:
    mutable std::mutex mutex_;
    std::queue<T> queue_;
    std::condition_variable cv_;
    bool stopped_ = false;
    size_t maxSize_ = 0;
};
