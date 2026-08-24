#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "asr/asr_engine.h"
#include "pipeline/ordered_result_buffer.h"

namespace {

using fcitx::AsrEngine;
using fcitx::AsrResult;
using fcitx::AsrSession;
using fcitx::AsrSessionStart;
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
    AsrSessionStart StartSession() override {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        auto cancelled = CancelOldestSessionIfLimitReachedLocked();
        const auto sessionId = nextSessionId_++;
        auto session = std::make_shared<TestSession>(sessionId);
        sessions_[sessionId] = session;
        retainedSessions_.push_back(session);
        return {session, cancelled};
    }
    const char* Name() const override { return "test"; }

    size_t SessionCount() {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        return sessions_.size();
    }

private:
    std::vector<std::shared_ptr<TestSession>> retainedSessions_;
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

bool TestSkipDiscardsCachedAndLateResults() {
    OrderedResultBuffer buffer;
    buffer.Reset(1);

    auto ready = buffer.Submit(Result(2, "B cached partial"), false);
    if (!Check(ready.empty(), "B partial must wait for A")) return false;

    ready = buffer.Skip(2);
    if (!Check(ready.empty(), "skipping future B must still wait for A")) {
        return false;
    }

    ready = buffer.Submit(Result(2, "B late final"), true);
    if (!Check(ready.empty(), "late B result must be discarded")) return false;

    ready = buffer.Submit(Result(1, "A"), true);
    return Check(ready.size() == 1,
                 "skipped B must not release cached or late results") &&
           Check(ready[0].text == "A", "only A may be released");
}

bool TestSessionLimitCancelsOldestSession() {
    TestEngine engine;
    auto first = engine.StartSession();
    engine.StartSession();
    engine.StartSession();
    auto fourth = engine.StartSession();

    return Check(fourth.cancelledSessionId ==
                     first.session->GetState()->sessionId,
                 "the oldest session must be cancelled") &&
           Check(first.session->GetState()->cancelled,
                 "the oldest session must receive Cancel") &&
           Check(engine.SessionCount() == AsrEngine::kMaxActiveSessions,
                 "the engine must retain exactly the configured limit");
}

bool TestReportedCancellationUnblocksCompletedMiddleSession() {
    TestEngine engine;
    auto first = engine.StartSession();
    engine.StartSession();
    engine.StartSession();

    OrderedResultBuffer buffer;
    buffer.Reset(1);
    buffer.Submit(Result(2, "B"), true);
    buffer.Submit(Result(3, "C"), true);

    auto fourth = engine.StartSession();
    if (!Check(fourth.cancelledSessionId ==
                   first.session->GetState()->sessionId,
               "StartSession must report the session it actually cancelled")) {
        return false;
    }

    auto ready = buffer.Skip(1);
    return Check(ready.size() == 2,
                 "skipping the reported oldest session must release B and C") &&
           Check(ready[0].text == "B" && ready[1].text == "C",
                 "completed middle sessions must retain their order");
}

} // namespace

int main() {
    return TestResultsAreReleasedInSpeechOrder() &&
                   TestLlmResultBlocksLaterUtteranceUntilFinal() &&
                   TestSkippedUtteranceUnblocksLaterResult() &&
                   TestSkipDiscardsCachedAndLateResults() &&
                   TestSessionLimitCancelsOldestSession() &&
                   TestReportedCancellationUnblocksCompletedMiddleSession()
               ? 0
               : 1;
}
