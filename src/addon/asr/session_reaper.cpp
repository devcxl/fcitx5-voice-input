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
    reapQueue_.Push(nullptr); // wake up reaper loop
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
}

void SessionReaper::Add(std::shared_ptr<AsrSession> session) {
    reapQueue_.Push(std::move(session));
}

void SessionReaper::DrainAll() {
    reapQueue_.Clear();
    reapQueue_.Push(nullptr); // wake up in case it's blocked
    std::this_thread::sleep_for(50ms);
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
    }
}

} // namespace fcitx
