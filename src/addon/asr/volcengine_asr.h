#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "asr_engine.h"
#include "utils/thread_safe_queue.h"

namespace fcitx {

class VolcengineStreamingAsrEngine : public AsrEngine {
public:
    VolcengineStreamingAsrEngine();
    ~VolcengineStreamingAsrEngine() override;

    VolcengineStreamingAsrEngine(const VolcengineStreamingAsrEngine&) = delete;
    VolcengineStreamingAsrEngine& operator=(const VolcengineStreamingAsrEngine&) = delete;

    bool Init(const Config& config) override;
    void Start() override;
    void FeedAudio(const float* pcm, size_t frames) override;
    void Stop() override;
    const char* Name() const override { return "volcengine"; }

private:
    void WorkerLoop();

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

    ThreadSafeQueue<std::vector<int16_t>> audioChunks_;
    std::unique_ptr<std::thread> workerThread_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> finished_{false};
};

} // namespace fcitx
