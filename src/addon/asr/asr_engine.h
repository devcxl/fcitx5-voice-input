#pragma once

#include <string>
#include <functional>
#include <vector>

namespace fcitx {

/**
 * ASR engine interface.
 *
 * Supports cloud providers via OpenAI-compatible API.
 * All implementations must be thread-safe: Start()/FeedAudio()/Stop()
 * follow an online session lifecycle.
 */
class AsrEngine {
public:
    struct Config {
        // Common
        std::string modelName;

        // Sherpa-onnx (local)
        std::string modelPath;
        int numThreads = 4;

        // OpenAI-compatible (cloud)
        std::string apiEndpoint;
        std::string apiKey;
        std::string language = "zh";

        // Volcengine Doubao streaming ASR (cloud)
        std::string authMode;
        std::string appKey;
        std::string accessKey;
        std::string resourceId;
        int chunkMs = 200;
        bool enableItN = true;
        bool enablePunc = true;
        bool enableDdc = false;
        bool enableNonstream = true;
        int endWindowMs = 800;
    };

    // Result callback — called from worker thread, caller must dispatch
    // to main thread (e.g. via EventDispatcher).
    using ResultCallback = std::function<void(const std::string& text, bool isFinal)>;
    using ErrorCallback = std::function<void(const std::string& error)>;

    virtual ~AsrEngine() = default;

    // Initialize the engine with config.
    // Returns false if initialization fails.
    virtual bool Init(const Config& config) = 0;

    // Start a new recognition session (clean state).
    virtual void Start() = 0;

    // Feed PCM audio data (16kHz mono F32) for recognition.
    // Non-blocking: audio is queued internally.
    virtual void FeedAudio(const float* pcm, size_t frames) = 0;

    // Signal end of input, trigger final result.
    // Non-blocking: processing continues on worker thread.
    virtual void Stop() = 0;

    // Unique name for this engine.
    virtual const char* Name() const = 0;

    // Set callbacks.
    void SetResultCallback(ResultCallback cb) { resultCb_ = std::move(cb); }
    void SetErrorCallback(ErrorCallback cb) { errorCb_ = std::move(cb); }

protected:
    ResultCallback resultCb_;
    ErrorCallback errorCb_;
};

} // namespace fcitx
