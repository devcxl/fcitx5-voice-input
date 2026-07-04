#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <fcitx-utils/log.h>

namespace fcitx {

/// 一次语音识别会话，对应 VAD 检测到的一句话。
/// 实现必须启用 enable_shared_from_this，worker 线程捕获
/// shared_ptr<State>，绝不捕获裸 this。
class AsrSession : public std::enable_shared_from_this<AsrSession> {
public:
    /// 线程安全的共享状态。
    /// worker 线程持有 shared_ptr<State>，即使 Session 被析构也安全。
    struct State {
        std::atomic<bool> cancelled{false};
        std::atomic<bool> finished{false};
        uint64_t sessionId{0};
    };

    using ResultCallback =
        std::function<void(const std::string& text, bool isFinal)>;
    using ErrorCallback = std::function<void(const std::string& error)>;

    virtual ~AsrSession() = default;

    /// 送入音频数据（16kHz mono F32）。非阻塞。
    virtual void FeedAudio(const float* pcm, size_t frames) = 0;

    /// 标记语音结束。永不阻塞。
    /// 引擎内部启动最终识别流程，完成后通过 resultCb_ 回调。
    virtual void End() = 0;

    /// 强制取消。永不阻塞。
    /// 设 cancelled 标志 + 主动 close socket + 超时轮询。
    virtual void Cancel() = 0;

    /// 供 SessionReaper 调用的阻塞 join，带超时。
    /// 超时后若线程仍在跑 → 日志 Error 后 detach（已知风险，curl 无强制中断）。
    virtual void JoinWithTimeout(std::chrono::milliseconds timeout) = 0;

    std::shared_ptr<State> GetState() const { return state_; }

    void SetResultCallback(ResultCallback cb) { resultCb_ = std::move(cb); }
    void SetErrorCallback(ErrorCallback cb) { errorCb_ = std::move(cb); }

protected:
    std::shared_ptr<State> state_ = std::make_shared<State>();
    ResultCallback resultCb_;
    ErrorCallback errorCb_;
};

} // namespace fcitx
