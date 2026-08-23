#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "types.h"

namespace fcitx {

// 将并发 ASR 回调按语音段创建顺序交付给主线程。
class OrderedResultBuffer {
public:
    void Reset(uint64_t firstUtteranceId) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
        nextUtteranceId_ = firstUtteranceId;
    }

    std::vector<AsrResult> Submit(AsrResult result, bool terminal) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (result.utteranceId < nextUtteranceId_) {
            return {};
        }

        auto& pending = pending_[result.utteranceId];
        if (result.isPartial && !pending.results.empty() &&
            pending.results.back().isPartial) {
            pending.results.back() = std::move(result);
        } else {
            pending.results.push_back(std::move(result));
        }
        pending.terminal = pending.terminal || terminal;
        return DrainReadyLocked();
    }

    std::vector<AsrResult> Skip(uint64_t utteranceId) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (utteranceId < nextUtteranceId_) {
            return {};
        }

        pending_[utteranceId].terminal = true;
        return DrainReadyLocked();
    }

private:
    struct PendingUtterance {
        std::deque<AsrResult> results;
        bool terminal = false;
    };

    std::vector<AsrResult> DrainReadyLocked() {
        std::vector<AsrResult> ready;
        while (true) {
            auto it = pending_.find(nextUtteranceId_);
            if (it == pending_.end()) {
                break;
            }

            auto& pending = it->second;
            while (!pending.results.empty()) {
                ready.push_back(std::move(pending.results.front()));
                pending.results.pop_front();
            }
            if (!pending.terminal) {
                break;
            }

            pending_.erase(it);
            ++nextUtteranceId_;
        }
        return ready;
    }

    std::mutex mutex_;
    std::map<uint64_t, PendingUtterance> pending_;
    uint64_t nextUtteranceId_ = 1;
};

} // namespace fcitx
