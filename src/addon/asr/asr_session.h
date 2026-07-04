#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace fcitx {

class AsrSession : public std::enable_shared_from_this<AsrSession> {
public:
    struct State {
        std::atomic<bool> cancelled{false};
        std::atomic<bool> finished{false};
        uint64_t sessionId{0};
    };

    using ResultCallback =
        std::function<void(const std::string& text, bool isFinal,
                           uint64_t sessionId)>;
    using ErrorCallback = std::function<void(const std::string& error)>;

    virtual ~AsrSession() = default;

    virtual void FeedAudio(const float* pcm, size_t frames) = 0;
    virtual void End() = 0;
    virtual void Cancel() = 0;
    virtual void JoinWithTimeout(std::chrono::milliseconds timeout) = 0;

    /// Start worker thread. Must be called after shared_ptr exists.
    virtual void StartWorker() = 0;

    std::shared_ptr<State> GetState() const { return state_; }

    void SetResultCallback(ResultCallback cb) { resultCb_ = std::move(cb); }
    void SetErrorCallback(ErrorCallback cb) { errorCb_ = std::move(cb); }

protected:
    std::shared_ptr<State> state_ = std::make_shared<State>();
    ResultCallback resultCb_;
    ErrorCallback errorCb_;
};

} // namespace fcitx
