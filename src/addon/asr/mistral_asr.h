#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "asr_engine.h"
#include "asr_session.h"
#include "utils/thread_safe_queue.h"

namespace fcitx {

/// Mistral Realtime 流式实时转录会话。
///
/// 协议与 OpenAI Realtime（realtime_asr.cpp）同构但事件名不同：
///   - 客户端事件：input_audio.append（base64 PCM s16le 16kHz，无需重采样）/
///                  input_audio.flush / input_audio.end / session.update
///   - 服务端事件：session.created / transcription.text.delta /
///                  transcription.segment / transcription.done / error
/// delta 事件驱动 preedit 增量（resultCb_(partial, false)），
/// transcription.done 后上报最终结果（resultCb_(full, true)）。
/// 支持周期 flush 兜底与断线重连（保持同一 sessionId）。
class MistralAsrSession : public AsrSession {
public:
    MistralAsrSession(const AsrEngine::Config& config,
                      AsrSession::ErrorCallback errorCb,
                      uint64_t sessionId);
    ~MistralAsrSession() override;

    void FeedAudio(const float* pcm, size_t frames) override;
    void End() override;
    void Cancel() override;
    void JoinWithTimeout(std::chrono::milliseconds timeout) override;
    void StartWorker() override;

private:
    void WorkerLoop();

    // Config snapshot (copied at construction, safe for worker)
    std::string endpoint_;
    std::string apiKey_;
    std::string modelName_;
    int commitIntervalMs_ = 5000;
    int targetStreamingDelayMs_ = 0;
    static constexpr int kMaxReconnectAttempts = 3;
    static constexpr std::chrono::seconds kSessionMaxDuration =
        std::chrono::seconds(30 * 60);

    std::shared_ptr<ThreadSafeQueue<std::vector<int16_t>>> audioChunks_;
    std::unique_ptr<std::thread> workerThread_;
};

class MistralAsrEngine : public AsrEngine {
public:
    bool Init(const Config& config) override;
    AsrSessionStart StartSession() override;
    const char* Name() const override { return "mistral-realtime"; }

private:
    Config config_;
};

} // namespace fcitx
