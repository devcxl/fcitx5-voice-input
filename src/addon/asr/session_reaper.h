#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <fcitx-utils/log.h>

#include "asr/asr_session.h"
#include "utils/thread_safe_queue.h"

namespace fcitx {

/// 专用线程，负责 join 已 End/Cancel 的 AsrSession。
/// Pipeline 将结束的 Session 移入 reapQueue_，Reaper 在后台线程
/// 执行 JoinWithTimeout，完成后释放 shared_ptr。
///
/// 避免 Pipeline 线程被阻塞，也避免在主线程 detach。
class SessionReaper {
public:
    SessionReaper();
    ~SessionReaper();

    SessionReaper(const SessionReaper&) = delete;
    SessionReaper& operator=(const SessionReaper&) = delete;

    /// 将一个已 End/Cancel 的 Session 交由 Reaper 回收。
    void Add(std::shared_ptr<AsrSession> session);

    /// 回收所有剩余 Session。用于 Pipeline Stop/Abort。
    void DrainAll();

    bool IsRunning() const { return running_.load(); }

private:
    void ReaperLoop();

    ThreadSafeQueue<std::shared_ptr<AsrSession>> reapQueue_;
    std::unique_ptr<std::thread> thread_;
    std::atomic<bool> running_{false};
};

} // namespace fcitx
