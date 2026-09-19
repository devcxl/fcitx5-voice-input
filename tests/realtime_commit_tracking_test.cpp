// Realtime commit 追踪与 error 事件回归测试（GitHub issue #44）。
//
// 背景：Realtime 后端用 `commitsInFlight` 判断「收到的 completed 是否是最终 item」：
// `isFinalItem = endCommitSent && commitsInFlight <= 1`。但该计数只在 `completed` 与
// `transcription.failed` 上递减，服务端返回的 `error` 事件被明确忽略，因此任何一次
// error（例如对空 buffer 的 `input_audio_buffer_commit_empty`）都会让计数永久虚高，
// 最终的 completed 被判定为「非最终」，会话只能等兜底超时才收尾。
//
// issue 实测：服务端在 End commit 后约 200ms 内就补发了最终 completed（内容
// "最终文本"），客户端却等了 30037ms。
//
// 本测试覆盖：
//   1. RealtimeCommitTracker 的判定语义（含 error 后仍能立即判定最终）；
//   2. 服务端确认 ack 缺失时，不得退化为「等满兜底超时」；
//   3. 空闲超时常量与 End 总预算的关系（兜底时间 ≤ 5s）。

#include <chrono>
#include <iostream>
#include <string>

#include "asr/utils/realtime_commit_tracker.h"
#include "asr/utils/ws_deadline.h"

namespace {

using namespace std::chrono_literals;
using fcitx::kEndIdleTimeout;
using fcitx::RealtimeCommitTracker;
using fcitx::WsDeadline;
using fcitx::WsEndBudget;

bool Check(bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

// ── 正常路径 ───────────────────────────────────────────────────

/// 服务端确认 End commit：只有该 item 的 completed 被判定为最终。
bool TestServerAckedEndItemIsFinal() {
    RealtimeCommitTracker tracker;
    tracker.OnCommitted("item_periodic", false);
    tracker.MarkEndCommitSent();

    // 周期 commit 的 completed 先到（即使 ack 顺序相反也应正确）
    if (!Check(!tracker.OnCompleted("item_periodic"),
               "a periodic commit item must not be treated as final")) {
        return false;
    }

    tracker.OnCommitted("item_end", true);
    return Check(tracker.OnCompleted("item_end"),
                 "the server-acked End item must be treated as final") &&
           Check(tracker.InFlightCount() == 0,
                 "the final item must be removed from in-flight tracking");
}

/// 未发出 End commit 时，任何 completed 都不是最终项。
bool TestCompletedBeforeEndCommitIsNotFinal() {
    RealtimeCommitTracker tracker;
    tracker.OnCommitted("item_1", false);
    return Check(!tracker.OnCompleted("item_1"),
                 "a completed before End commit must not be final");
}

// ── issue #44 核心：error 事件后计数不虚高 ──────────────────────

/// 周期 commit 被服务端 error 拒绝（不产生 item，也不回 completed），
/// 之后 End commit 的 completed 必须立即被判定为最终。
bool TestErrorOnPeriodicCommitDoesNotDelayFinal() {
    RealtimeCommitTracker tracker;

    // 周期 commit：本地已发送但服务端回 error（例如空 buffer 的 commit_empty），
    // 因此没有 input_audio_buffer.committed，也没有 completed/failed。
    // （修复前的计数器会在这里 +1 且永不递减。）
    tracker.MarkEndCommitSent();
    tracker.OnCommitted("item_end", true);

    if (!Check(tracker.InFlightCount() == 1,
               "only the server-acked End item may be in flight")) {
        return false;
    }
    return Check(tracker.OnCompleted("item_end"),
                 "the final completed must be recognized immediately after a "
                 "rejected periodic commit (no 30s fallback wait)");
}

/// 服务端完全不回 committed ack（仅回 completed）：在途集合为空即视为最终，
/// 不得退化为「等满兜底超时」。
bool TestMissingAckStillRecognizesFinal() {
    RealtimeCommitTracker tracker;
    tracker.MarkEndCommitSent();

    return Check(tracker.OnCompleted("item_unknown"),
                 "with no ack and an empty in-flight set, the completed must be "
                 "treated as the final item (no fallback wait)") &&
           Check(tracker.EndItemId().empty(),
                 "an unacked End item id stays unknown");
}

/// End item 未知时：已确认的周期 item 不得被判为最终；
/// 未曾在在途集合中出现过的 item（服务端未 ack 的 End commit）才是最终。
bool TestUnknownEndItemDistinguishesPeriodicFromEnd() {
    RealtimeCommitTracker tracker;
    tracker.OnCommitted("item_periodic", false);
    tracker.MarkEndCommitSent();

    if (!Check(!tracker.OnCompleted("item_periodic"),
               "a server-acked periodic item must not be treated as final")) {
        return false;
    }
    return Check(tracker.OnCompleted("item_end_unacked"),
                 "an unacked item appearing after End commit is the final one");
}

/// failed 事件只移除对应 item，不影响判定其他在途项。
bool TestFailedRemovesOnlyItsItem() {
    RealtimeCommitTracker tracker;
    tracker.OnCommitted("item_periodic", false);
    tracker.MarkEndCommitSent();
    tracker.OnCommitted("item_end", true);

    tracker.OnFailed("item_periodic");
    if (!Check(tracker.InFlightCount() == 1,
               "failed must remove only its own item")) {
        return false;
    }
    return Check(tracker.OnCompleted("item_end"),
                 "the End item must still be final after an unrelated failed");
}

/// 空 item_id 的 completed（服务端未带该字段）：不误删在途项，按集合是否清空判定。
bool TestEmptyItemIdDoesNotMisjudge() {
    RealtimeCommitTracker tracker;
    tracker.OnCommitted("item_periodic", false);
    tracker.MarkEndCommitSent();

    if (!Check(!tracker.OnCompleted(""),
               "an empty item_id must not bypass the in-flight check")) {
        return false;
    }
    tracker.OnCompleted("item_periodic");
    return Check(tracker.OnCompleted(""),
                 "once in-flight drains, an empty-item_id completed may be final");
}

// ── 空闲超时与兜底时长 ────────────────────────────────────────

/// 空闲超时必须 ≤ 5s，且明显小于 End 总预算与 reaper 的 15s join。
bool TestIdleTimeoutIsBounded() {
    return Check(kEndIdleTimeout <= 5000ms,
                 "the End idle timeout must be at most 5s (issue #44 acceptance)") &&
           Check(kEndIdleTimeout < WsEndBudget::kDefaultTotal,
                 "the idle timeout must be smaller than the End total budget") &&
           Check(WsEndBudget::kDefaultTotal < 15000ms,
                 "the End total budget must stay below the reaper's 15s join");
}

/// WsDeadline 按剩余时间收缩：End 预算的剩余量必须能约束单次发送。
bool TestEndBudgetShrinksSendDeadline() {
    WsEndBudget endBudget;
    if (!Check(!endBudget.Armed(), "an unarmed End budget must report unarmed")) {
        return false;
    }

    endBudget.Arm(600ms);
    const auto deadline = endBudget.Deadline();
    if (!Check(deadline.Budget() <= 600ms,
               "the send deadline must not exceed the End budget")) {
        return false;
    }

    // 幂等：重复 Arm 不得重置截止时刻
    const auto remaining = endBudget.Remaining();
    endBudget.Arm(60000ms);
    return Check(endBudget.Remaining() <= remaining + 50ms,
                 "Arm must be idempotent (no deadline extension)") &&
           Check(deadline.Expired() == false || deadline.Remaining().count() == 0,
                 "the derived deadline must report a consistent state");
}

} // namespace

int main() {
    return TestServerAckedEndItemIsFinal() &&
                   TestCompletedBeforeEndCommitIsNotFinal() &&
                   TestErrorOnPeriodicCommitDoesNotDelayFinal() &&
                   TestMissingAckStillRecognizesFinal() &&
                   TestUnknownEndItemDistinguishesPeriodicFromEnd() &&
                   TestFailedRemovesOnlyItsItem() &&
                   TestEmptyItemIdDoesNotMisjudge() &&
                   TestIdleTimeoutIsBounded() &&
                   TestEndBudgetShrinksSendDeadline()
               ? 0
               : 1;
}
