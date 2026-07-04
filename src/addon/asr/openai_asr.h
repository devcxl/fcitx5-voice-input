#pragma once

#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <string>

#include "asr_engine.h"
#include "asr_session.h"

namespace fcitx {

class OpenaiAsrSession : public AsrSession {
public:
    OpenaiAsrSession(const AsrEngine::Config& config,
                     AsrSession::ResultCallback resultCb,
                     AsrSession::ErrorCallback errorCb,
                     uint64_t sessionId);
    ~OpenaiAsrSession() override;

    void FeedAudio(const float* pcm, size_t frames) override;
    void End() override;
    void Cancel() override;
    void JoinWithTimeout(std::chrono::milliseconds timeout) override;

private:
    void TranscribeWorker(std::vector<float> pcm);
    std::string DoHttpRequest(const std::vector<uint8_t>& wavData);

    std::string apiEndpoint_;
    std::string apiKey_;
    std::string modelName_;
    std::string language_;
    std::vector<float> pcmBuffer_;
    std::mutex bufferMutex_;
    std::unique_ptr<std::thread> workerThread_;
};

class OpenaiAsrEngine : public AsrEngine {
public:
    bool Init(const Config& config) override;
    std::shared_ptr<AsrSession> StartSession() override;
    const char* Name() const override { return "openai-compat"; }

private:
    Config config_;
};

} // namespace fcitx
