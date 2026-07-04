#include "asr_engine.h"

namespace fcitx {

AsrEngine::~AsrEngine() {
    CancelAllSessions();
}

void AsrEngine::CancelAllSessions() {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    for (auto& [id, weak] : sessions_) {
        auto session = weak.lock();
        if (session) {
            session->Cancel();
        }
    }
    sessions_.clear();
}

} // namespace fcitx
