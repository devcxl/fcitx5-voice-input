#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "asr/mistral_asr.h"

namespace mistral_fixture {

inline constexpr uint64_t kSessionId = 73;
inline constexpr const char* kApiKey = "test-key";

struct Outcome {
    std::atomic<int> errors{0};
    std::atomic<int> finals{0};
    std::atomic<int> partials{0};
    std::atomic<uint64_t> finalSessionId{0};
    std::mutex mutex;
    std::string errorText;
    std::string finalText;
};

inline void RecordError(Outcome& out, const std::string& text) {
    out.errors.fetch_add(1);
    std::lock_guard lock(out.mutex);
    if (out.errorText.empty()) out.errorText = text;
}

inline void RecordResult(Outcome& out, const std::string& text, bool isFinal, uint64_t sid) {
    if (!isFinal) {
        out.partials.fetch_add(1);
        return;
    }
    out.finals.fetch_add(1);
    out.finalSessionId.store(sid);
    std::lock_guard lock(out.mutex);
    out.finalText = text;
}

inline std::string ErrorText(Outcome& out) {
    std::lock_guard lock(out.mutex);
    return out.errorText;
}

inline std::string FinalText(Outcome& out) {
    std::lock_guard lock(out.mutex);
    return out.finalText;
}

// Callbacks own observations so a failed timeout test cannot leave a detached
// worker holding references to the test's stack. Modes inject callback faults.
inline std::shared_ptr<fcitx::MistralAsrSession> MakeSession(
    const std::string& url, int throwMode, const std::shared_ptr<Outcome>& out) {
    fcitx::AsrEngine::Config config;
    config.apiEndpoint = url;
    config.apiKey = kApiKey;
    config.modelName = "voxtral-mini-transcribe-realtime-2602";
    config.commitIntervalMs = 1000;
    auto session = std::make_shared<fcitx::MistralAsrSession>(
        config, [out, throwMode](const std::string& text) {
            RecordError(*out, text);
            if (throwMode == 3) throw std::runtime_error("private error detail");
        }, kSessionId);
    session->SetResultCallback([out, throwMode](const std::string& text, bool isFinal,
                                               uint64_t sid) {
        RecordResult(*out, text, isFinal, sid);
        if (isFinal && throwMode == 2) throw std::runtime_error("private final detail");
        if (!isFinal && throwMode == 1) throw std::runtime_error("private partial detail");
    });
    return session;
}

} // namespace mistral_fixture
