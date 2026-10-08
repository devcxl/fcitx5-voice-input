#pragma once

#include <cstdio>
#include <exception>
#include <typeinfo>

#include <fcitx-utils/log.h>

#include "asr/asr_session.h"

namespace fcitx {

// Diagnostics must not include exception.what(): protocol/callback exceptions
// may contain credentials or transcription data. Retain category and session.
inline void LogAsrBoundaryFailure(const char* tag, uint64_t sessionId,
                                  const char* category) noexcept {
    try {
        FCITX_ERROR() << tag << " " << category << " session=" << sessionId;
    } catch (...) {
        // Even an allocation failure in the logger must not abort the host.
        std::fprintf(stderr, "%s %s session=%llu\n", tag, category,
                     static_cast<unsigned long long>(sessionId));
    }
}

inline void NotifyAsrError(const AsrSession::ErrorCallback& callback,
                           const std::string& message, const char* tag,
                           uint64_t sessionId) noexcept {
    if (!callback) return;
    try {
        callback(message);
    } catch (...) {
        LogAsrBoundaryFailure(tag, sessionId, "error callback threw");
    }
}

// Both WS providers share this thread contract, not their protocol semantics.
// A callback may perform side effects before throwing: never retry a final.
template<typename Work, typename PublishFailure>
void RunAsrWorker(AsrSession::State& state, const char* tag,
                  const AsrSession::ErrorCallback& errorCallback,
                  Work work, PublishFailure publishFailure) noexcept {
    bool failed = false;
    try {
        work();
    } catch (const std::exception& exception) {
        failed = true;
        LogAsrBoundaryFailure(tag, state.sessionId, typeid(exception).name());
    } catch (...) {
        failed = true;
        LogAsrBoundaryFailure(tag, state.sessionId, "unknown worker exception");
    }
    if (failed && !state.cancelled.load()) {
        try {
            NotifyAsrError(errorCallback, "ASR worker exception", tag, state.sessionId);
        } catch (...) {
            LogAsrBoundaryFailure(tag, state.sessionId, "error notification failed");
        }
        try {
            publishFailure();
        } catch (...) {
            LogAsrBoundaryFailure(tag, state.sessionId, "final callback threw");
        }
    }
    state.finished.store(true);
    state.workerDone.store(true, std::memory_order_release);
}

} // namespace fcitx
