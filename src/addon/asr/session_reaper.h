#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <fcitx-utils/log.h>

#include "asr/asr_session.h"
#include "utils/thread_safe_queue.h"

namespace fcitx {

class SessionReaper {
public:
    SessionReaper();
    ~SessionReaper();

    SessionReaper(const SessionReaper&) = delete;
    SessionReaper& operator=(const SessionReaper&) = delete;

    void Add(std::shared_ptr<AsrSession> session);
    void DrainAll();
    bool IsRunning() const { return running_.load(); }

private:
    void ReaperLoop();

    ThreadSafeQueue<std::shared_ptr<AsrSession>> reapQueue_;
    std::unique_ptr<std::thread> thread_;
    std::atomic<bool> running_{false};
    std::atomic<size_t> pending_{0};
};

} // namespace fcitx
