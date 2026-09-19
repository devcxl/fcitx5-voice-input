#pragma once

#include <atomic>
#include <chrono>

#include <curl/curl.h>

namespace fcitx {

/// 一次 WS 操作的 wall-clock 预算（发送或等待服务端响应）。
///
/// 为何必须由调用方限时：libcurl 的 `CURLOPT_TIMEOUT` 只约束传输阶段，
/// connect-only 建立后的 `curl_ws_send` / `curl_ws_recv` 不受其约束
/// （issue #42 实测：对端不读 socket 时 8MB 单帧在 8s 内重试 788 次、1 字节未发出）。
/// 上游卡死/限流/反代 hang 时 `curl_ws_send` 持续 `CURLE_AGAIN`，发送循环永不退出，
/// worker 线程会在 `SessionReaper::JoinWithTimeout`（15s）超时后被 detach 泄漏。
///
/// 预算按值复制共享同一截止时刻，不会因复制而重置计时，因此可在「同一逻辑操作」
/// 的多次发送与等待之间传递（例如 End 路径的 flush → commit → 等待终态）。
class WsDeadline {
public:
    static constexpr std::chrono::milliseconds kDefaultBudget{10000};

    explicit WsDeadline(std::chrono::milliseconds budget = kDefaultBudget)
        : deadline_(std::chrono::steady_clock::now() + budget), budget_(budget) {}

    bool Expired() const { return std::chrono::steady_clock::now() >= deadline_; }

    std::chrono::milliseconds Remaining() const {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline_ - std::chrono::steady_clock::now());
        return left.count() > 0 ? left : std::chrono::milliseconds::zero();
    }

    std::chrono::milliseconds Budget() const { return budget_; }

private:
    std::chrono::steady_clock::time_point deadline_;
    std::chrono::milliseconds budget_;
};

/// End 路径总预算：从 `End()` 被调用起算，覆盖尾部音频 flush、commit/end 发送、
/// 等待服务端终态，以及其间可能发生的重连。
///
/// 为何需要它：单次发送预算只约束「一次发送」，但 End 之后仍可能出现
/// 「发送失败 → 重连 → 再发送失败」的循环（每次各耗一份预算），累计时长可超过
/// `SessionReaper::JoinWithTimeout`（15s），worker 依旧会被 detach 泄漏（issue #42 的
/// 级联放大）。End 预算把整条收尾路径的耗时封顶在一个上限内。
///
/// 线程安全：`End()` 由 pipeline 线程调用，worker 线程读取，因此内部用原子量。
class WsEndBudget {
public:
    static constexpr std::chrono::milliseconds kDefaultTotal{8000};

    /// 武装预算；幂等，重复调用不会重置截止时刻。
    void Arm(std::chrono::milliseconds total = kDefaultTotal) {
        if (armed_.exchange(true, std::memory_order_acq_rel)) return;
        const auto deadline = std::chrono::steady_clock::now() + total;
        deadlineTicks_.store(deadline.time_since_epoch().count(),
                             std::memory_order_release);
        totalMs_.store(total.count(), std::memory_order_release);
    }

    bool Armed() const { return armed_.load(std::memory_order_acquire); }

    bool Expired() const {
        return Armed() && Remaining().count() <= 0;
    }

    std::chrono::milliseconds Remaining() const {
        const std::chrono::steady_clock::time_point deadline{
            std::chrono::steady_clock::duration{
                deadlineTicks_.load(std::memory_order_acquire)}};
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        return left.count() > 0 ? left : std::chrono::milliseconds::zero();
    }

    std::chrono::milliseconds Total() const {
        return std::chrono::milliseconds(totalMs_.load(std::memory_order_acquire));
    }

    /// 供发送使用的截止时刻（未武装时返回一份全新的默认预算）。
    WsDeadline Deadline() const {
        return Armed() ? WsDeadline(Remaining()) : WsDeadline{};
    }

private:
    std::atomic<bool> armed_{false};
    std::atomic<std::chrono::steady_clock::rep> deadlineTicks_{0};
    std::atomic<std::chrono::steady_clock::rep> totalMs_{0};
};

/// WS 操作的中止条件（发送与建连共用）。
///
/// `cancelled` 在任何路径都应中止操作（用户主动取消）。
/// `finished` 仅在「非 End 路径」的操作上生效——`End()` 置位 finished 之后仍需发送
/// 尾部音频 flush 与 commit/end 事件，那些调用传 nullptr。推流过程中的 append/commit
/// 一旦发现 finished 就应立即放弃：否则 End() 后仍会为一次注定作废的推流耗费整个
/// 发送预算，并在失败后重连再阻塞一轮（issue #42 实测的级联放大）。
/// 建连同理：End() 时若尚未建立连接，重试注定没有意义，应立即中止并交出终态。
struct WsAbort {
    const std::atomic<bool>* cancelled = nullptr;
    const std::atomic<bool>* finished = nullptr;

    bool Requested() const {
        return (cancelled && cancelled->load(std::memory_order_acquire)) ||
               (finished && finished->load(std::memory_order_acquire));
    }

    /// 仅响应取消（End 路径的 flush/commit 使用）。
    static WsAbort CancelOnly(const std::atomic<bool>& cancelled) {
        return WsAbort{&cancelled, nullptr};
    }

    /// 响应取消或语音结束（讲话过程中的推流、WS 建连使用）。
    static WsAbort CancelOrFinished(const std::atomic<bool>& cancelled,
                                    const std::atomic<bool>& finished) {
        return WsAbort{&cancelled, &finished};
    }
};

/// libcurl `CURLOPT_XFERINFOFUNCTION` 适配：clientp 指向 `WsAbort`，置位即中止。
/// connect-only 的建连阶段也走 curl 的 perform 循环，因此同样受此约束。
inline int WsAbortProgressCallback(void* clientp, curl_off_t, curl_off_t,
                                   curl_off_t, curl_off_t) {
    const auto* abort = static_cast<const WsAbort*>(clientp);
    return abort && abort->Requested() ? 1 : 0;
}

} // namespace fcitx
