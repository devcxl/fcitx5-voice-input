// 保序闸门停滞放行回归测试（GitHub issue #43）。
//
// 背景：OrderedResultBuffer 是严格串行闸门——nextUtteranceId_ 对应的语音段没有
// terminal 结果时，后续已完成的语音段结果全部不下发（Submit 只从 nextUtteranceId_
// 开始 DrainReadyLocked）。因此任何一段 ASR 会话卡住/丢失终态，都会把「单段延迟」
// 放大成「整条链路不可见」：用户看到的是「输入法卡住了」，而不是「这一段晚一点出来」。
//
// 实测复现（issue #43）：第 1 个连接完全不应答、后续连接对每次 commit 立即回
// completed，客户端按真实 Begin/End 时序驱动 5 段语音，结果 5 段累计交付 0 条。
//
// 本测试覆盖三个层面：
//   1. OrderedResultBuffer 的停滞判定与放行（可注入时间基准，确定性）；
//   2. ResultCoordinator 端到端：卡死段 + 后续段的结果按序交付且不丢失；
//   3. 正常路径不误伤：阈值内的慢段不会被放行。

#include <chrono>
#include <iostream>
#include <thread>
#include <memory>
#include <string>
#include <vector>

#include "pipeline/ordered_result_buffer.h"
#include "pipeline/result_coordinator.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using fcitx::AsrResult;
using fcitx::OrderedResultBuffer;
using fcitx::ResultCoordinator;

bool Check(bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

AsrResult Result(uint64_t utteranceId, const std::string& text,
                 bool partial = false, bool isError = false,
                 uint64_t generation = 1) {
    AsrResult result;
    result.utteranceId = utteranceId;
    result.sessionId = utteranceId;
    result.generation = generation;
    result.text = text;
    result.isPartial = partial;
    result.isError = isError;
    return result;
}

/// 文本序列是否为期望值（用于断言交付顺序与内容）。
bool TextSequence(const std::vector<AsrResult>& results,
                  const std::vector<std::string>& expected) {
    if (results.size() != expected.size()) return false;
    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i].text != expected[i]) return false;
    }
    return true;
}

// ── OrderedResultBuffer：停滞判定与放行 ────────────────────────

/// 正常路径：队首有部分结果、未超过阈值时，增量立即可见但闸门不前进。
bool TestPartialVisibleWithoutStall() {
    OrderedResultBuffer buffer(1000ms);
    const auto t0 = Clock::time_point{};

    auto ready = buffer.SubmitAt(Result(1, "A partial", true), false, t0);
    if (!Check(TextSequence(ready, {"A partial"}),
               "a partial result must be visible immediately")) {
        return false;
    }

    ready = buffer.SubmitAt(Result(2, "B", false, false), false, t0 + 500ms);
    return Check(ready.empty(),
                 "the gate must not advance while the head has no terminal result");
}

/// issue #43 核心场景：队首卡死（无终态），后续段已完成 → 停滞后放行。
bool TestStalledHeadUnblocksLaterUtterances() {
    OrderedResultBuffer buffer(1000ms);
    const auto t0 = Clock::time_point{};

    // 段 1 卡死：只来了一个 partial，之后没有任何回调
    auto ready = buffer.SubmitAt(Result(1, "段一 partial", true), false, t0);
    if (!Check(TextSequence(ready, {"段一 partial"}),
               "the stuck utterance partial must still be visible")) {
        return false;
    }

    // 段 2..5 正常完成
    for (uint64_t id = 2; id <= 5; ++id) {
        ready = buffer.SubmitAt(Result(id, "段" + std::to_string(id)), true,
                                t0 + 100ms);
        if (!Check(ready.empty(),
                   "later utterances must wait while the head is unresolved")) {
            return false;
        }
    }

    // 停滞阈值内不误伤
    ready = buffer.ExpireStaleAt(t0 + 900ms);
    if (!Check(ready.empty(), "a head inside the stall budget must not be expired")) {
        return false;
    }

    // 超过阈值：放行，卡死段变为一条超时错误，后续 4 段按序交付
    ready = buffer.ExpireStaleAt(t0 + 1000ms);
    if (!Check(ready.size() == 5,
               "expiring a stalled head must release it and all later results")) {
        return false;
    }
    return Check(ready[0].utteranceId == 1 && ready[0].isError,
                 "the stalled utterance must be released as a timeout error") &&
           Check(ready[1].text == "段2" && ready[2].text == "段3" &&
                     ready[3].text == "段4" && ready[4].text == "段5",
                 "later utterances must be released in speech order") &&
           Check(ready[0].generation == 1,
                 "the timeout result must keep the utterance generation so the "
                 "engine-side filter still delivers it");
}

/// 队首完全没有结果（连接从未应答）时，按「后续段首次到达」起算停滞。
bool TestHeadWithoutAnyResultUsesBlockedSince() {
    OrderedResultBuffer buffer(1000ms);
    const auto t0 = Clock::time_point{};

    // 段 1 从未产生任何结果；段 2 完成
    auto ready = buffer.SubmitAt(Result(2, "段2"), true, t0);
    if (!Check(ready.empty(), "段2 must wait for the silent head")) return false;

    ready = buffer.ExpireStaleAt(t0 + 999ms);
    if (!Check(ready.empty(), "must not expire before the stall budget")) {
        return false;
    }

    ready = buffer.ExpireStaleAt(t0 + 1000ms);
    return Check(ready.size() == 2,
                 "a silent head blocking others must be expired and release them") &&
           Check(ready[0].utteranceId == 1 && ready[0].isError,
                 "the silent head must be released as an error result") &&
           Check(ready[1].utteranceId == 2 && ready[1].text == "段2",
                 "the blocked utterance must follow the timeout result") &&
           Check(ready[1].generation == 1,
                 "the timeout result must carry a usable generation");
}

/// 停滞放行后，卡死段的迟到结果必须被丢弃（不扰乱已推进的闸门）。
bool TestLateResultAfterExpiryIsDiscarded() {
    OrderedResultBuffer buffer(1000ms);
    const auto t0 = Clock::time_point{};

    buffer.SubmitAt(Result(1, "A partial", true), false, t0);
    buffer.SubmitAt(Result(2, "B"), true, t0 + 100ms);

    auto ready = buffer.ExpireStaleAt(t0 + 1000ms);
    if (!Check(ready.size() == 2, "expiry must release A(error) and B")) return false;

    // 迟到的段 1 终态：utteranceId < nextUtteranceId_（已推进到 3）
    ready = buffer.SubmitAt(Result(1, "A late"), true, t0 + 2000ms);
    return Check(ready.empty(), "a late result of an expired utterance is discarded");
}

/// 连续多段停滞：一次排空应能推进多个超时段。
bool TestMultipleStalledUtterancesAreReleased() {
    OrderedResultBuffer buffer(1000ms);
    const auto t0 = Clock::time_point{};

    buffer.SubmitAt(Result(1, "段一 partial", true), false, t0);
    buffer.SubmitAt(Result(2, "段二 partial", true), false, t0 + 10ms);
    buffer.SubmitAt(Result(3, "段3"), true, t0 + 20ms);

    auto ready = buffer.ExpireStaleAt(t0 + 5000ms);
    return Check(ready.size() == 3,
                 "a single drain must release every stalled head in sequence") &&
           Check(ready[0].utteranceId == 1 && ready[1].utteranceId == 2 &&
                     ready[2].utteranceId == 3,
                 "stalled utterances must be released in speech order") &&
           Check(ready[0].isError && ready[1].isError,
                 "both stalled utterances must be reported as timeout errors") &&
           Check(ready[2].text == "段3",
                 "the completed utterance must keep its real text");
}

/// 正常路径不误伤：阈值内到齐的连续段按原顺序交付（回归保护）。
bool TestNormalPathOrderIsPreserved() {
    OrderedResultBuffer buffer(1000ms);
    const auto t0 = Clock::time_point{};

    auto ready = buffer.SubmitAt(Result(1, "A raw"), false, t0);
    if (!Check(TextSequence(ready, {"A raw"}),
               "raw A must be visible before its final")) {
        return false;
    }

    ready = buffer.SubmitAt(Result(2, "B"), true, t0 + 100ms);
    if (!Check(ready.empty(), "B must wait for A's terminal result")) return false;

    ready = buffer.SubmitAt(Result(1, "A refined"), true, t0 + 900ms);
    return Check(TextSequence(ready, {"A refined", "B"}),
                 "A refined and B must be released in order") &&
           Check(ready[0].utteranceId == 1 && ready[1].utteranceId == 2,
                 "utterance ids must stay ordered");
}

/// Skip 语义不变（显式跳过仍然立即放行）。
bool TestExplicitSkipStillWorks() {
    OrderedResultBuffer buffer(1000ms);
    const auto t0 = Clock::time_point{};

    buffer.SubmitAt(Result(2, "B"), true, t0);
    auto ready = buffer.SkipAt(1, t0 + 10ms);
    return Check(ready.size() == 1 && ready[0].text == "B",
                 "an explicit skip must still release blocked results") &&
           Check(!ready[0].isError, "an explicit skip must not emit an error result");
}

// ── ResultCoordinator：端到端交付 ──────────────────────────────

/// 卡死会话不阻塞后续会话：ResultCoordinator 层的停滞放行。
bool TestCoordinatorReleasesBlockedSessions() {
    auto coordinator = std::make_shared<ResultCoordinator>();
    coordinator->Start(1, 1);

    // 5 段语音，全部注册；段 1 卡死（只给 partial，不给终态）
    for (uint64_t id = 1; id <= 5; ++id) {
        coordinator->RegisterSession(id, {1, id});
    }

    coordinator->HandleAsrResult("段一 partial", false, 1);
    for (uint64_t id = 2; id <= 5; ++id) {
        coordinator->HandleAsrResult("段" + std::to_string(id), true, id);
    }

    if (!Check(coordinator->ResultQueue().Size() == 1,
               "only the stuck utterance partial is deliverable so far")) {
        return false;
    }

    // 阈值内：不误伤
    coordinator->ExpireStale();
    if (!Check(coordinator->ResultQueue().Size() == 1,
               "ExpireStale inside the budget must not release anything")) {
        return false;
    }
    return true;
}

/// 卡死会话停滞放行后，后续所有会话的结果都按序可交付（issue #43 的端到端形态）。
bool TestCoordinatorDeliversAllAfterStall() {
    auto coordinator = std::make_shared<ResultCoordinator>(50ms);
    coordinator->Start(1, 1);
    for (uint64_t id = 1; id <= 5; ++id) {
        coordinator->RegisterSession(id, {1, id});
    }

    coordinator->HandleAsrResult("段一 partial", false, 1);
    for (uint64_t id = 2; id <= 5; ++id) {
        coordinator->HandleAsrResult("段" + std::to_string(id), true, id);
    }

    AsrResult result;
    std::vector<std::string> delivered;
    while (coordinator->ResultQueue().TryPop(result)) {
        if (!result.isError) delivered.push_back(result.text);
    }
    if (!Check(delivered.size() == 1 && delivered[0] == "段一 partial",
               "only the stuck partial is deliverable before the stall budget")) {
        return false;
    }

    // 超过停滞阈值后周期性检查放行（ResultCoordinator(50ms) 构造）
    std::this_thread::sleep_for(80ms);
    coordinator->ExpireStale();

    delivered.clear();
    bool sawTimeoutError = false;
    while (coordinator->ResultQueue().TryPop(result)) {
        if (result.isError) {
            sawTimeoutError = true;
            continue;
        }
        delivered.push_back(result.text);
    }
    return Check(sawTimeoutError,
                 "the stalled utterance must be reported as an error (visible to the user)") &&
           Check(delivered.size() == 4,
                 "all 4 later utterances must be delivered after the stall expires") &&
           Check(delivered[0] == "段2" && delivered[3] == "段5",
                 "later utterances must be delivered in speech order");
}

} // namespace

int main() {
    return TestPartialVisibleWithoutStall() &&
                   TestStalledHeadUnblocksLaterUtterances() &&
                   TestHeadWithoutAnyResultUsesBlockedSince() &&
                   TestLateResultAfterExpiryIsDiscarded() &&
                   TestMultipleStalledUtterancesAreReleased() &&
                   TestNormalPathOrderIsPreserved() &&
                   TestExplicitSkipStillWorks() &&
                   TestCoordinatorReleasesBlockedSessions() &&
                   TestCoordinatorDeliversAllAfterStall()
               ? 0
               : 1;
}
