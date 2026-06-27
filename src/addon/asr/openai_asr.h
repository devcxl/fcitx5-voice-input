#pragma once

#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <string>

#include "asr_engine.h"

namespace fcitx {

/**
 * ASR engine for OpenAI Whisper API and compatible providers
 * (Groq, Together AI, DeepSeek, etc.).
 *
 * User configures the endpoint, API key, and model name at runtime.
 * Audio is sent as a WAV file via multipart/form-data POST request.
 */
class OpenaiCompatAsrEngine : public AsrEngine {
public:
    OpenaiCompatAsrEngine();
    ~OpenaiCompatAsrEngine() override;

    OpenaiCompatAsrEngine(const OpenaiCompatAsrEngine&) = delete;
    OpenaiCompatAsrEngine& operator=(const OpenaiCompatAsrEngine&) = delete;

    bool Init(const Config& config) override;
    void Start() override;
    void FeedAudio(const float* pcm, size_t frames) override;
    void Stop() override;
    const char* Name() const override { return "openai-compat"; }

private:
    void TranscribeWorker();

    // HTTP POST multipart/form-data to the API endpoint
    std::string DoHttpRequest(const std::vector<uint8_t>& wavData);
    static std::string NormalizeChinese(const std::string& text);

    // Config
    std::string apiEndpoint_;
    std::string apiKey_;
    std::string modelName_;
    std::string language_;

    // Audio buffer (accumulated during recording)
    std::vector<float> pcmBuffer_;

    // Thread management
    std::unique_ptr<std::thread> workerThread_;
    std::atomic<bool> cancelled_{false};
};

} // namespace fcitx
