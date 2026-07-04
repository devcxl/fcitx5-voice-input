#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "asr_engine.h"
#include "asr_session.h"
#include "utils/thread_safe_queue.h"

namespace fcitx {

class VolcengineAsrSession : public AsrSession {
public:
    VolcengineAsrSession(const AsrEngine::Config& config,
                         AsrSession::ErrorCallback errorCb,
                         uint64_t sessionId);
    ~VolcengineAsrSession() override;

    void FeedAudio(const float* pcm, size_t frames) override;
    void End() override;
    void Cancel() override;
    void JoinWithTimeout(std::chrono::milliseconds timeout) override;
    void StartWorker() override;

private:
    void WorkerLoop();

    // Config snapshot (copied at construction, safe for worker)
    std::string endpoint_;
    std::string authMode_;
    std::string apiKey_;
    std::string appKey_;
    std::string accessKey_;
    std::string resourceId_;
    std::string modelName_;
    int chunkMs_ = 200;
    bool enableItN_ = true;
    bool enablePunc_ = true;
    bool enableDdc_ = false;
    bool enableNonstream_ = true;
    int endWindowMs_ = 800;

    std::shared_ptr<ThreadSafeQueue<std::vector<int16_t>>> audioChunks_;
    std::unique_ptr<std::thread> workerThread_;
};

class VolcengineAsrEngine : public AsrEngine {
public:
    bool Init(const Config& config) override;
    std::shared_ptr<AsrSession> StartSession() override;
    const char* Name() const override { return "volcengine"; }

private:
    Config config_;
};

} // namespace fcitx
