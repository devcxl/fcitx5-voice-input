#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "config/config.h"
#include "capture/audio_capture.h"
#include "vad/vad.h"
#include "asr/asr_engine.h"
#include "llm/llm_client.h"
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
    void SetGeneration(uint64_t gen) { generation_.store(gen); }

    // Lifecycle
    void Start();
    void Stop();
    void Abort();
    bool IsRunning() const { return running_.load(); }

    // Main thread polls this
    ThreadSafeQueue<AsrResult>& ResultQueue() { return resultQueue_; }

    void SetConfig(const VoiceInputConfig& config);

private:
    bool StartCapture();
    void AsrWorkerLoop();

    // Queues
    ThreadSafeQueue<AudioFrame> frameQueue_;
    ThreadSafeQueue<Utterance> utteranceQueue_;
    ThreadSafeQueue<AsrResult> resultQueue_;

    // Workers
    std::unique_ptr<VADWorker> vadWorker_;
    std::unique_ptr<std::thread> asrThread_;

    // Capture
    std::unique_ptr<AudioCapture> capture_;

    // ASR
    std::unique_ptr<AsrEngine> asrEngine_;

    // LLM
    std::unique_ptr<LLMClient> llmClient_;
    bool llmStream_ = true;

    // State
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> generation_{0};
    uint64_t utteranceCounter_{0};

    // Config
    VoiceInputConfig config_;

    // Callback
    ResultCallback resultCb_;
};

} // namespace fcitx
