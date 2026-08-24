#include "result_coordinator.h"

#include <algorithm>
#include <utility>

namespace fcitx {
namespace {

AsrResult MakeResult(const std::string& text, uint64_t sessionId,
                     const ResultCoordinator::SessionMetadata& metadata) {
    AsrResult result;
    result.text = text;
    result.generation = metadata.generation;
    result.sessionId = sessionId;
    result.utteranceId = metadata.utteranceId;
    return result;
}

} // namespace

void ResultCoordinator::Start(uint64_t firstUtteranceId,
                              uint64_t generation) {
    std::shared_ptr<LLMClient> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        accepting_ = true;
        generation_ = generation;
        sessions_.clear();
        orderedResults_.Reset(firstUtteranceId);
        resultQueue_.Clear();
        client = llmClient_;
    }
    if (client) client->Activate(generation);
}

std::shared_ptr<LLMClient> ResultCoordinator::DeactivateLocked(
    uint64_t nextUtteranceId, bool permanently) {
    accepting_ = false;
    closed_ = closed_ || permanently;
    sessions_.clear();
    orderedResults_.Reset(nextUtteranceId);
    resultQueue_.Clear();
    return llmClient_;
}

void ResultCoordinator::Pause(uint64_t nextUtteranceId) {
    std::shared_ptr<LLMClient> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = DeactivateLocked(nextUtteranceId, false);
    }
    if (client) client->Cancel();
}

void ResultCoordinator::Close(uint64_t nextUtteranceId) {
    std::shared_ptr<LLMClient> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = DeactivateLocked(nextUtteranceId, true);
    }
    if (client) client->Cancel();
    CloseNotifications();
}

void ResultCoordinator::SetResultCallback(ResultCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (callbacksOpen_) resultCallback_ = std::move(callback);
}

void ResultCoordinator::SetLLMClient(std::shared_ptr<LLMClient> client) {
    std::shared_ptr<LLMClient> previous;
    uint64_t generation = 0;
    bool activate = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = std::exchange(llmClient_, client);
        generation = generation_;
        activate = accepting_ && !closed_;
    }
    if (previous) previous->Cancel();
    if (client && activate) client->Activate(generation);
}

void ResultCoordinator::SetLLMStream(bool stream) {
    std::lock_guard<std::mutex> lock(mutex_);
    llmStream_ = stream;
}

void ResultCoordinator::RegisterSession(uint64_t sessionId,
                                        SessionMetadata metadata) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (accepting_ && metadata.generation == generation_) {
        sessions_[sessionId] = metadata;
    }
}

std::optional<ResultCoordinator::ResultContext>
ResultCoordinator::TakeContext(uint64_t sessionId, bool isFinal) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_) return std::nullopt;
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) return std::nullopt;

    ResultContext context{it->second, llmClient_, llmStream_};
    if (isFinal) sessions_.erase(it);
    return context;
}

void ResultCoordinator::HandleAsrResult(const std::string& text, bool isFinal,
                                        uint64_t sessionId) {
    auto context = TakeContext(sessionId, isFinal);
    if (!context) return;

    if (isFinal) {
        HandleFinal(text, sessionId, *context);
    } else if (!text.empty()) {
        auto partial = MakeResult(text, sessionId, context->metadata);
        partial.isPartial = true;
        Submit(std::move(partial), false);
    }
}

void ResultCoordinator::HandleFinal(const std::string& text,
                                    uint64_t sessionId,
                                    const ResultContext& context) {
    if (text.empty()) {
        auto error = MakeResult({}, sessionId, context.metadata);
        error.isError = true;
        Submit(std::move(error), true);
        return;
    }

    auto raw = MakeResult(text, sessionId, context.metadata);
    Submit(std::move(raw), !context.llmClient);
    if (context.llmClient) Refine(text, sessionId, context);
}

void ResultCoordinator::Refine(const std::string& text, uint64_t sessionId,
                               const ResultContext& context) {
    if (context.llmStream) {
        RefineStream(text, sessionId, context);
        return;
    }

    bool completed = false;
    auto self = shared_from_this();
    context.llmClient->Process(
        text, context.metadata.generation,
        [self, &completed, sessionId,
         metadata = context.metadata](const std::string& processed) {
            completed = true;
            auto result = MakeResult(processed, sessionId, metadata);
            result.isLLMRefined = true;
            self->Submit(std::move(result), true);
        });
    if (!completed) {
        auto fallback = MakeResult(text, sessionId, context.metadata);
        fallback.isLLMRefined = true;
        Submit(std::move(fallback), true);
    }
}

void ResultCoordinator::RefineStream(const std::string& text,
                                     uint64_t sessionId,
                                     const ResultContext& context) {
    bool completed = false;
    auto self = shared_from_this();
    context.llmClient->ProcessStream(
        text, context.metadata.generation,
        [self, sessionId,
         metadata = context.metadata](const std::string& partialText) {
            auto partial = MakeResult(partialText, sessionId, metadata);
            partial.isLLMRefined = true;
            partial.isPartial = true;
            self->Submit(std::move(partial), false);
        },
        [self, &completed, sessionId,
         metadata = context.metadata](const std::string& finalText) {
            completed = true;
            auto result = MakeResult(finalText, sessionId, metadata);
            result.isLLMRefined = true;
            self->Submit(std::move(result), true);
        });
    if (!completed) {
        auto fallback = MakeResult(text, sessionId, context.metadata);
        fallback.isLLMRefined = true;
        Submit(std::move(fallback), true);
    }
}

std::optional<std::string> ResultCoordinator::EnqueueLocked(
    std::vector<AsrResult> ready) {
    if (ready.empty()) return std::nullopt;
    std::string notification = ready.back().text;
    for (auto& result : ready) resultQueue_.Push(std::move(result));
    return notification;
}

void ResultCoordinator::Submit(AsrResult result, bool terminal) {
    std::optional<std::string> notification;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_ || result.generation != generation_) return;
        notification = EnqueueLocked(
            orderedResults_.Submit(std::move(result), terminal));
    }
    if (notification) Notify(*notification);
}

void ResultCoordinator::SkipSession(uint64_t sessionId) {
    std::optional<std::string> notification;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(sessionId);
        if (!accepting_ || it == sessions_.end()) return;
        const uint64_t utteranceId = it->second.utteranceId;
        sessions_.erase(it);
        notification = EnqueueLocked(orderedResults_.Skip(utteranceId));
    }
    if (notification) Notify(*notification);
}

void ResultCoordinator::SkipUtterance(uint64_t utteranceId) {
    std::optional<std::string> notification;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) return;
        notification = EnqueueLocked(orderedResults_.Skip(utteranceId));
    }
    if (notification) Notify(*notification);
}

void ResultCoordinator::SkipAllSessions() {
    std::optional<std::string> notification;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) return;
        std::vector<uint64_t> utteranceIds;
        utteranceIds.reserve(sessions_.size());
        for (const auto& [sessionId, metadata] : sessions_) {
            utteranceIds.push_back(metadata.utteranceId);
        }
        sessions_.clear();
        std::sort(utteranceIds.begin(), utteranceIds.end());
        for (uint64_t utteranceId : utteranceIds) {
            auto ready = orderedResults_.Skip(utteranceId);
            if (auto latest = EnqueueLocked(std::move(ready))) {
                notification = std::move(latest);
            }
        }
    }
    if (notification) Notify(*notification);
}

void ResultCoordinator::Notify(const std::string& text) {
    ResultCallback callback;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (!callbacksOpen_ || !resultCallback_) return;
        callback = resultCallback_;
        ++callbacksInFlight_;
    }
    try {
        callback(text);
    } catch (...) {
        FinishNotification();
        throw;
    }
    FinishNotification();
}

void ResultCoordinator::FinishNotification() {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    --callbacksInFlight_;
    if (!callbacksOpen_ && callbacksInFlight_ == 0) callbackCv_.notify_all();
}

void ResultCoordinator::CloseNotifications() {
    std::unique_lock<std::mutex> lock(callbackMutex_);
    callbacksOpen_ = false;
    resultCallback_ = {};
    callbackCv_.wait(lock, [this] { return callbacksInFlight_ == 0; });
}

} // namespace fcitx
