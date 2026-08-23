#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "config/voiceinput-config.h"
#include "capture/audio_capture.h"
#include "vad/vad.h"
#include "asr/asr_engine.h"
#include "asr/asr_session.h"
#include "asr/session_reaper.h"
#include "llm/llm_client.h"
#include "pipeline/ordered_result_buffer.h"
#include "types.h"
#include "utils/thread_safe_queue.h"

namespace fcitx {

class Pipeline {
public:
    using ResultCallback = std::function<void(const std::string& text)>;

    Pipeline();
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void Init(const VoiceInputConfig& config);
    void SetAsrEngine(std::unique_ptr<AsrEngine> engine);
    void SetLLMClient(std::unique_ptr<LLMClient> client);
    void SetLLMStream(bool stream) { llmStream_ = stream; }
    void SetResultCallback(ResultCallback cb);
    void SetVadStatusCallback(VADWorker::VadStatusCallback cb);
    void SetGeneration(uint64_t gen) { generation_.store(gen); }

    void Start();
    void Stop();
    void Abort();
    bool IsRunning() const { return running_.load(); }

    ThreadSafeQueue<AsrResult>& ResultQueue() { return resultQueue_; }

    void SetConfig(const VoiceInputConfig& config);

private:
    struct SessionMetadata {
        uint64_t generation = 0;
        uint64_t utteranceId = 0;
    };

    bool StartCapture();
    void AsrDispatcherLoop();
    void SubmitOrderedResult(AsrResult result, bool terminal);
    void SkipUtterance(uint64_t utteranceId);
    void ResetOrderedResults();

    // Queues（带容量上限：超限丢最旧，防止异常路径下内存无界增长）
    // 1024 帧 ≈ 32s 音频缓冲；256 事件 ≈ 256 段语音；128 结果
    ThreadSafeQueue<AudioFrame> frameQueue_{1024};
    ThreadSafeQueue<SpeechEvent> speechEventQueue_{256};
    ThreadSafeQueue<AsrResult> resultQueue_{128};

    // Workers
    std::unique_ptr<VADWorker> vadWorker_;
    std::unique_ptr<std::thread> asrThread_;

    // Capture
    std::unique_ptr<AudioCapture> capture_;

    // ASR session management
    std::shared_ptr<AsrEngine> asrEngine_;
    std::shared_ptr<AsrSession> activeSession_;
    uint64_t activeSessionId_{0};
    std::unordered_map<uint64_t, SessionMetadata> sessionGenerationMap_;
    std::mutex sessionMapMutex_;   // 保护 sessionGenerationMap_（worker/ASR/主线程三方）
    std::mutex engineMutex_;       // 保护 asrEngine_ 指针替换与使用
    std::unique_ptr<SessionReaper> reaper_;

    // ASR streaming batching
    std::vector<float> pendingAsrAudio_;

    // LLM
    std::unique_ptr<LLMClient> llmClient_;
    bool llmStream_ = true;

    // State
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> utteranceCounter_{0};
    OrderedResultBuffer orderedResults_;
    std::mutex orderedResultMutex_;
    // 回调守卫：Abort 后置 false，引擎 worker 线程的异步回调据此丢弃
    std::shared_ptr<std::atomic<bool>> resultGuard_ =
        std::make_shared<std::atomic<bool>>(true);

    // Config
    VoiceInputConfig config_;

    // Callback
    ResultCallback resultCb_;
};

} // namespace fcitx
