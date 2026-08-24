#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "llm/llm_client.h"
#include "pipeline/ordered_result_buffer.h"
#include "types.h"
#include "utils/thread_safe_queue.h"

namespace fcitx {

// 管理异步 ASR/LLM 回调的完整共享状态，允许其安全晚于 Pipeline 退出。
class ResultCoordinator final
    : public std::enable_shared_from_this<ResultCoordinator> {
public:
    using ResultCallback = std::function<void(const std::string& text)>;

    struct SessionMetadata {
        uint64_t generation = 0;
        uint64_t utteranceId = 0;
    };

    void Start(uint64_t firstUtteranceId, uint64_t generation);
    void Pause(uint64_t nextUtteranceId);
    void Close(uint64_t nextUtteranceId);

    // 回调必须短小，且不得递归调用 Close()。
    void SetResultCallback(ResultCallback callback);
    void SetLLMClient(std::shared_ptr<LLMClient> client);
    void SetLLMStream(bool stream);

    void RegisterSession(uint64_t sessionId, SessionMetadata metadata);
    void SkipSession(uint64_t sessionId);
    void SkipUtterance(uint64_t utteranceId);
    void SkipAllSessions();
    void HandleAsrResult(const std::string& text, bool isFinal,
                         uint64_t sessionId);

    ThreadSafeQueue<AsrResult>& ResultQueue() { return resultQueue_; }

private:
    struct ResultContext {
        SessionMetadata metadata;
        std::shared_ptr<LLMClient> llmClient;
        bool llmStream = true;
    };

    std::optional<ResultContext> TakeContext(uint64_t sessionId, bool isFinal);
    void HandleFinal(const std::string& text, uint64_t sessionId,
                     const ResultContext& context);
    void Refine(const std::string& text, uint64_t sessionId,
                const ResultContext& context);
    void RefineStream(const std::string& text, uint64_t sessionId,
                      const ResultContext& context);
    void Submit(AsrResult result, bool terminal);
    std::optional<std::string> EnqueueLocked(std::vector<AsrResult> ready);
    std::shared_ptr<LLMClient> DeactivateLocked(uint64_t nextUtteranceId,
                                                bool permanently);

    void Notify(const std::string& text);
    void FinishNotification();
    void CloseNotifications();

    std::mutex mutex_;
    bool accepting_ = false;
    bool closed_ = false;
    uint64_t generation_ = 0;
    std::unordered_map<uint64_t, SessionMetadata> sessions_;
    OrderedResultBuffer orderedResults_;
    ThreadSafeQueue<AsrResult> resultQueue_;
    std::shared_ptr<LLMClient> llmClient_;
    bool llmStream_ = true;

    std::mutex callbackMutex_;
    std::condition_variable callbackCv_;
    ResultCallback resultCallback_;
    bool callbacksOpen_ = true;
    size_t callbacksInFlight_ = 0;
};

} // namespace fcitx
