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
#include "utils/ws_frame_sender.h"

namespace fcitx {

/// Mistral Realtime 流式实时转录会话。
///
/// Provider 独立协议；共享传输/线程边界，不共享 OpenAI commit/item 语义：
///   - 客户端事件：input_audio.append（base64 PCM s16le 16kHz，无需重采样）/
///                  input_audio.flush / input_audio.end / session.update
///   - 服务端事件：session.created / transcription.text.delta /
///                  transcription.segment / transcription.done / error
/// delta 事件驱动 preedit 增量（resultCb_(partial, false)），
/// 等待 session.created 后推流；transcription.done 的 text 为权威最终全文。
/// 协议错误一次空 final；断连/End 超时以合法累积文本兜底，取消不发 final。
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
    void PublishFinal(const std::string& transcript) {
        if (state_->cancelled || finalPublished_) return;
        // Callback side effects may happen before it throws; never retry it.
        finalPublished_ = true;
        if (resultCb_) resultCb_(transcript, true, state_->sessionId);
    }
    // Kept provider-local: unlike OpenAI, Mistral has no commit/item tracker.
    bool finalPublished_ = false; // worker-only, includes exceptional exits

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

    // End 路径总预算（跨线程：End() 武装，worker 读取）
    WsEndBudget endBudget_;
    // 建连中止条件（clientp 生命周期需覆盖 curl_easy_perform）
    WsAbort connectAbort_;
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
