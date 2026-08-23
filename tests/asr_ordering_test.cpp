#include <chrono>
#include <iostream>
#include <memory>
#include <string>

#include "asr/asr_engine.h"
#include "pipeline/ordered_result_buffer.h"

namespace {

using fcitx::AsrEngine;
using fcitx::AsrResult;
using fcitx::AsrSession;
using fcitx::OrderedResultBuffer;

bool Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

class TestSession final : public AsrSession {
public:
    explicit TestSession(uint64_t sessionId) {
        state_->sessionId = sessionId;
    }

    void FeedAudio(const float*, size_t) override {}
    void End() override {}
    void Cancel() override { state_->cancelled = true; }
    void JoinWithTimeout(std::chrono::milliseconds) override {}
    void StartWorker() override {}
};

class TestEngine final : public AsrEngine {
public:
    bool Init(const Config&) override { return true; }
    std::shared_ptr<AsrSession> StartSession() override { return {}; }
    const char* Name() const override { return "test"; }

    std::shared_ptr<TestSession> AddSession() {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        const auto sessionId = nextSessionId_++;
        auto session = std::make_shared<TestSession>(sessionId);
        sessions_[sessionId] = session;
        return session;
    }

    uint64_t CancelOldestAtLimit() {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        return CancelOldestSessionIfLimitReachedLocked().value_or(0);
    }

    size_t SessionCount() {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        return sessions_.size();
    }
};

AsrResult Result(uint64_t utteranceId, std::string text) {
    AsrResult result;
    result.utteranceId = utteranceId;
    result.text = std::move(text);
    return result;
}

bool TestResultsAreReleasedInSpeechOrder() {
    OrderedResultBuffer buffer;
    buffer.Reset(1);

    auto ready = buffer.Submit(Result(2, "B"), true);
    if (!Check(ready.empty(), "B must wait for A")) return false;

    ready = buffer.Submit(Result(1, "A"), true);
    return Check(ready.size() == 2, "A and B must both be released") &&
           Check(ready[0].text == "A", "A must be released first") &&
           Check(ready[1].text == "B", "B must be released second");
}

bool TestLlmResultBlocksLaterUtteranceUntilFinal() {
    OrderedResultBuffer buffer;
    buffer.Reset(1);

    auto ready = buffer.Submit(Result(1, "A raw"), false);
    if (!Check(ready.size() == 1 && ready[0].text == "A raw",
               "raw A result must be visible")) {
        return false;
    }

    ready = buffer.Submit(Result(2, "B"), true);
    if (!Check(ready.empty(), "B must wait for A LLM final")) return false;

    ready = buffer.Submit(Result(1, "A refined"), true);
    return Check(ready.size() == 2, "A refined and B must be released") &&
           Check(ready[0].text == "A refined", "A refined must precede B") &&
           Check(ready[1].text == "B", "B must follow A refined");
}

bool TestSkippedUtteranceUnblocksLaterResult() {
    OrderedResultBuffer buffer;
    buffer.Reset(1);

    auto ready = buffer.Submit(Result(2, "B"), true);
    if (!Check(ready.empty(), "B must wait until A is resolved")) return false;

    ready = buffer.Skip(1);
    return Check(ready.size() == 1, "skipping A must release B") &&
           Check(ready[0].text == "B", "B must be released after A is skipped");
}

bool TestSessionLimitCancelsOldestSession() {
    TestEngine engine;
    auto first = engine.AddSession();
    auto second = engine.AddSession();
    auto third = engine.AddSession();

    const auto cancelledId = engine.CancelOldestAtLimit();
    return Check(cancelledId == first->GetState()->sessionId,
                 "the oldest session must be cancelled") &&
           Check(first->GetState()->cancelled,
                 "the oldest session must receive Cancel") &&
           Check(engine.SessionCount() == 2,
                 "the cancelled session must be removed before adding another");
}

} // namespace

int main() {
    return TestResultsAreReleasedInSpeechOrder() &&
                   TestLlmResultBlocksLaterUtteranceUntilFinal() &&
                   TestSkippedUtteranceUnblocksLaterResult() &&
                   TestSessionLimitCancelsOldestSession()
               ? 0
               : 1;
}
