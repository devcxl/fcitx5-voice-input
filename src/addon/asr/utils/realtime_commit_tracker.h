#pragma once

#include <chrono>
#include <cstdint>
#include <set>
#include <string>

namespace fcitx {

/// End 等待期间的「服务端空闲」超时：距最后一个服务端事件超过该时长即判定上游
/// 不会再给出终态，直接以已累积文本兜底。
///
/// 为何不是固定总时长（issue #44）：原实现在 End 后用固定 30s 硬等待，即使上游
/// 早已无响应也要等满；以「最后一个服务端事件」为基准的空闲超时对上游偶发延迟更
/// 鲁棒，也不会误伤正常慢速上游（正常路径下服务端在毫秒级持续回包）。
inline constexpr std::chrono::milliseconds kEndIdleTimeout{5000};

/// 追踪 commit 产生的在途转写 item，并判定某一 completed 是否为 End 提交的最终 item。
///
/// 为何用 item 集合而非计数器（issue #44）：计数器只在 `completed` / `failed` 上
/// 递减，服务端返回 `error`（例如对空 buffer 的 `input_audio_buffer_commit_empty`）
/// 时不递减 → 计数永久虚高 → 最终的 `completed` 被误判为「非最终 item」→ 只能等
/// 兜底超时才上屏。集合按服务端确认的 `item_id` 增删，天然对重复/丢失事件更鲁棒。
class RealtimeCommitTracker {
public:
    /// 收到 `input_audio_buffer.committed`（服务端确认 commit）时登记 item。
    /// 以服务端确认而非本地发送为准，避免把被服务端拒绝的 commit 计入。
    /// `endCommitSent` 为真时记下首个确认的 item 作为 End item。
    void OnCommitted(const std::string& itemId, bool endCommitSent) {
        if (itemId.empty()) return;
        itemsInFlight_.insert(itemId);
        if (endCommitSent && endItemId_.empty()) endItemId_ = itemId;
    }

    /// 收到 `conversation.item.input_audio_transcription.completed` 时判定是否最终 item，
    /// 并在判定后移除该 item。
    ///
    /// 判定规则（按优先级）：
    /// - End commit 未发出 → 非最终（周期 commit 的结果）；
    /// - 已知 End item id 且本次事件带 item_id → 精确匹配；
    /// - 未知 End item id 且事件带 item_id → 若该 id **不在**在途集合中，说明它是
    ///   服务端未确认过的 commit（被 error 拒绝，或该部署不回 committed ack）产生的，
    ///   在 End 之后出现即为最终；否则它是已确认的周期 item，非最终。
    ///   这条规则正是 issue #44 的修复点：否则会退化成「等满兜底超时」。
    /// - 事件不带 item_id（或未知 End id 且集合为空）→ 以在途集合是否清空判定。
    bool OnCompleted(const std::string& itemId) {
        const bool isFinal = IsFinalCompletedItem(itemId);
        Remove(itemId);
        return isFinal;
    }

    /// 收到 `...transcription.failed` 时移除对应 item（不做最终判定）。
    void OnFailed(const std::string& itemId) { Remove(itemId); }

    /// End commit 已送达（本地发送成功）时调用，用于后续登记 End item。
    void MarkEndCommitSent() { endCommitSent_ = true; }

    bool EndCommitSent() const { return endCommitSent_; }

    /// 在途 item 数（仅用于观测/测试断言）。
    size_t InFlightCount() const { return itemsInFlight_.size(); }

    /// 已确认的 End item id（空表示服务端尚未确认，或 End commit 未被接受）。
    const std::string& EndItemId() const { return endItemId_; }

    void Reset() {
        itemsInFlight_.clear();
        endItemId_.clear();
        endCommitSent_ = false;
    }

private:
    bool IsFinalCompletedItem(const std::string& itemId) const {
        if (!endCommitSent_) return false;
        if (!endItemId_.empty() && !itemId.empty()) return itemId == endItemId_;
        if (itemId.empty()) return itemsInFlight_.empty();
        // 未知 End item id：未确认过的 item（不在在途集合）即为最终；
        // 已确认的周期 item 仍按非最终处理，避免用周期文本提前收尾。
        return itemsInFlight_.find(itemId) == itemsInFlight_.end();
    }

    void Remove(const std::string& itemId) {
        if (itemId.empty()) return;
        itemsInFlight_.erase(itemId);
        if (itemId == endItemId_) endItemId_.clear();
    }

    std::set<std::string> itemsInFlight_;
    std::string endItemId_;
    bool endCommitSent_ = false;
};

} // namespace fcitx
