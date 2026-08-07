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

/// OpenAI Realtime 流式实时转录会话。
///
/// 与 VolcengineAsrSession 结构对齐：StartWorker 即建 WS 持久连接，
/// FeedAudio 转 int16 入队，worker 线程内做 16k→24k 重采样、base64 推送、
/// 解析 delta/completed 事件。支持周期性 commit 兜底与断线重连（保持 sessionId）。
class RealtimeAsrSession : public AsrSession {
public:
    RealtimeAsrSession(const AsrEngine::Config& config,
                       AsrSession::ErrorCallback errorCb,
                       uint64_t sessionId);
    ~RealtimeAsrSession() override;

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
    std::string language_;
    int commitIntervalMs_ = 5000;
    static constexpr int kMaxReconnectAttempts = 3;
    static constexpr std::chrono::seconds kSessionMaxDuration = std::chrono::seconds(30 * 60);

    std::shared_ptr<ThreadSafeQueue<std::vector<int16_t>>> audioChunks_;
    std::unique_ptr<std::thread> workerThread_;
};

class RealtimeAsrEngine : public AsrEngine {
public:
    bool Init(const Config& config) override;
    std::shared_ptr<AsrSession> StartSession() override;
    const char* Name() const override { return "realtime"; }

private:
    Config config_;
};

} // namespace fcitx
