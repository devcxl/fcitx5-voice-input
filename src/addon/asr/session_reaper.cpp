#include "session_reaper.h"

#include <chrono>

using namespace std::chrono_literals;

namespace fcitx {

SessionReaper::SessionReaper() {
    running_ = true;
    thread_ = std::make_unique<std::thread>(&SessionReaper::ReaperLoop, this);
}

SessionReaper::~SessionReaper() {
    running_ = false;
    reapQueue_.Clear();
    reapQueue_.Push(nullptr);
    if (thread_ && thread_->joinable()) thread_->join();
}

void SessionReaper::Add(std::shared_ptr<AsrSession> session) {
    pending_++;
    reapQueue_.Push(std::move(session));
}

void SessionReaper::DrainAll() {
    reapQueue_.Clear();
    // Wait for currently joining session to finish
    auto deadline = std::chrono::steady_clock::now() + 16s;
    while (pending_.load() > 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    if (pending_.load() > 0) {
        FCITX_WARN() << "[voice-input:reaper] DrainAll timeout, "
                     << pending_.load() << " sessions left";
    }
}

void SessionReaper::ReaperLoop() {
    while (running_) {
        auto session = reapQueue_.Pop();
        if (!session) continue;
        if (!running_) break;

        uint64_t sid = session->GetState()->sessionId;
        FCITX_DEBUG() << "[voice-input:reaper] Joining session " << sid;
        session->JoinWithTimeout(15s);
        FCITX_INFO() << "[voice-input:reaper] Released session " << sid;
        pending_--;
    }
}

} // namespace fcitx
