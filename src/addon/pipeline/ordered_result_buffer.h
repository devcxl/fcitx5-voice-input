#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "types.h"

namespace fcitx {

// 将并发 ASR 回调按语音段创建顺序交付给主线程。
//
// 严格串行闸门会把「单段故障」放大成「整链路静默」（issue #43）：只要
// nextUtteranceId_ 对应的语音段没有 terminal 结果，后续已完成的语音段结果全部
// 不下发。因此这里额外提供**停滞放行**：
//
// - 队首语音段已有结果但长时间（maxStallMs）没有新进展 → 视为卡死；
// - 队首语音段完全没有结果、而后续语音段已在等待 → 从「后续段首次到达」起计时，
//   超过 maxStallMs 同样视为卡死；
// - 卡死时清掉该段已缓存内容，改为下推一条超时错误结果（generation/sessionId 沿用
//   该段既有结果，保证仍能通过引擎侧 generation 过滤并提示用户），随后推进闸门。
//
// 时间基准由调用方注入（*At 变体），便于确定性测试；默认使用 steady_clock。
class OrderedResultBuffer {
public:
    // 为何是 40s：合法的「队首静默」最长来自非流式 LLM 后处理（LLMClient 的
    // CURLOPT_TIMEOUT 为 30s），加上调度余量。ASR 侧 End 路径已有 8s 上界
    // （见 ws_deadline.h 的 WsEndBudget），故 40s 不会误伤正常慢路径。
    static constexpr std::chrono::milliseconds kDefaultMaxStall{40000};

    explicit OrderedResultBuffer(
        std::chrono::milliseconds maxStallMs = kDefaultMaxStall)
        : maxStallMs_(maxStallMs) {}

    void Reset(uint64_t firstUtteranceId) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
        nextUtteranceId_ = firstUtteranceId;
    }

    std::vector<AsrResult> Submit(AsrResult result, bool terminal) {
        return SubmitAt(std::move(result), terminal,
                        std::chrono::steady_clock::now());
    }

    std::vector<AsrResult> Skip(uint64_t utteranceId) {
        return SkipAt(utteranceId, std::chrono::steady_clock::now());
    }

    /// 放行已停滞的队首语音段。由调用方周期性触发（Pipeline 的 ASR 分发循环在
    /// 空闲时调用），因为「后续段已全部到达、此后不再有新回调」时没有提交动作来
    /// 触发检查。
    std::vector<AsrResult> ExpireStale() {
        return ExpireStaleAt(std::chrono::steady_clock::now());
    }

    // ── 可注入时间基准的变体（测试用） ────────────────────────────

    std::vector<AsrResult> SubmitAt(AsrResult result, bool terminal,
                                    std::chrono::steady_clock::time_point now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (result.utteranceId < nextUtteranceId_) {
            return {};
        }

        auto& pending = pending_[result.utteranceId];
        if (pending.skipped) {
            return {};
        }
        if (result.isPartial && !pending.results.empty() &&
            pending.results.back().isPartial) {
            pending.results.back() = std::move(result);
        } else {
            pending.results.push_back(std::move(result));
        }
        pending.terminal = pending.terminal || terminal;
        pending.lastSeen = now;
        return DrainReadyLocked(now);
    }

    std::vector<AsrResult> SkipAt(uint64_t utteranceId,
                                  std::chrono::steady_clock::time_point now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (utteranceId < nextUtteranceId_) {
            return {};
        }

        auto& pending = pending_[utteranceId];
        pending.results.clear();
        pending.skipped = true;
        pending.terminal = true;
        pending.lastSeen = now;
        return DrainReadyLocked(now);
    }

    std::vector<AsrResult> ExpireStaleAt(std::chrono::steady_clock::time_point now) {
        std::lock_guard<std::mutex> lock(mutex_);
        return DrainReadyLocked(now);
    }

private:
    struct PendingUtterance {
        std::deque<AsrResult> results;
        bool terminal = false;
        bool skipped = false;
        std::chrono::steady_clock::time_point lastSeen{};
    };

    // 队首语音段是否已停滞。必须持锁调用。
    // 判定基准：该段最后一次产生回调的时刻（即使那批增量已被交付），以及
    // 「最早的被阻塞后续段」两者中更早者。前者覆盖「段 1 刚出一个 partial 就死掉」，
    // 后者覆盖「段 1 完全不应答、但后续段已完成并被扣住」。
    bool HeadStalledLocked(std::chrono::steady_clock::time_point now) const {
        std::optional<std::chrono::steady_clock::time_point> since;
        auto it = pending_.find(nextUtteranceId_);
        if (it != pending_.end()) {
            // 已有终态的队首会在本轮被正常排空，不算停滞
            if (it->second.terminal) return false;
            since = it->second.lastSeen;
        }
        if (auto blocked = EarliestBlockedSinceLocked();
            blocked && (!since || *blocked < *since)) {
            since = blocked;
        }
        if (!since) return false;
        return now - *since >= maxStallMs_;
    }

    // 最早到达的、id 大于队首的语音段的时间戳（即闸门真正开始阻塞他人的时刻）。
    std::optional<std::chrono::steady_clock::time_point>
    EarliestBlockedSinceLocked() const {
        std::optional<std::chrono::steady_clock::time_point> earliest;
        auto it = pending_.upper_bound(nextUtteranceId_);
        for (; it != pending_.end(); ++it) {
            if (it->second.results.empty()) continue;
            if (!earliest || it->second.lastSeen < *earliest) {
                earliest = it->second.lastSeen;
            }
        }
        return earliest;
    }

    // 放行停滞的队首：清掉缓存增量，改为一条超时错误结果。
    void ExpireHeadLocked(std::chrono::steady_clock::time_point now) {
        auto& pending = pending_[nextUtteranceId_];

        AsrResult timeout;
        timeout.utteranceId = nextUtteranceId_;
        timeout.isError = true;
        timeout.isPartial = false;
        if (!pending.results.empty()) {
            // 沿用既有结果的 generation/sessionId，保证通过引擎侧 generation 过滤
            const AsrResult& last = pending.results.back();
            timeout.generation = last.generation;
            timeout.sessionId = last.sessionId;
        } else {
            // 队首从未产生结果：借用最早被阻塞段的元信息
            auto it = pending_.upper_bound(nextUtteranceId_);
            for (; it != pending_.end(); ++it) {
                if (!it->second.results.empty()) {
                    timeout.generation = it->second.results.back().generation;
                    timeout.sessionId = it->second.results.back().sessionId;
                    break;
                }
            }
        }

        pending.results.clear();
        pending.results.push_back(std::move(timeout));
        pending.terminal = true;
        pending.skipped = false;
        pending.lastSeen = now;
    }

    std::vector<AsrResult> DrainReadyLocked(
        std::chrono::steady_clock::time_point now) {
        std::vector<AsrResult> ready;
        while (true) {
            auto it = pending_.find(nextUtteranceId_);
            if (it == pending_.end()) {
                // 队首尚未产生任何结果：若它已阻塞他人过久则放行，否则等待
                if (HeadStalledLocked(now)) {
                    ExpireHeadLocked(now);
                    continue;
                }
                break;
            }

            auto& pending = it->second;
            if (!pending.terminal) {
                // 队首未终结：先交付其已缓存增量（preedit 需要立即可见），
                // 但闸门不能前进；若停滞过久则改为超时错误并放行。
                if (HeadStalledLocked(now)) {
                    ExpireHeadLocked(now);
                    continue;
                }
                while (!pending.results.empty()) {
                    ready.push_back(std::move(pending.results.front()));
                    pending.results.pop_front();
                }
                break;
            }

            while (!pending.results.empty()) {
                ready.push_back(std::move(pending.results.front()));
                pending.results.pop_front();
            }
            pending_.erase(it);
            ++nextUtteranceId_;
        }
        return ready;
    }

    std::mutex mutex_;
    std::map<uint64_t, PendingUtterance> pending_;
    uint64_t nextUtteranceId_ = 1;
    std::chrono::milliseconds maxStallMs_;
};

} // namespace fcitx
