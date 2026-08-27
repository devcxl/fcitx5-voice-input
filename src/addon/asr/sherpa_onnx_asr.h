#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "asr_engine.h"
#include "asr_session.h"
#include "utils/thread_safe_queue.h"

#ifdef HAVE_SHERPA_ONNX
#include <sherpa-onnx/c-api/c-api.h>
#endif

namespace fcitx {

#ifdef HAVE_SHERPA_ONNX
/// 线程安全封装 SherpaOnnxOnlineRecognizer 生命周期及并发解码操作。
/// 由 Engine 和所有 Session 共享持有，避免热重载时发生 UAF。
class SherpaRecognizerHolder {
public:
    explicit SherpaRecognizerHolder(const SherpaOnnxOnlineRecognizer* recognizer);
    ~SherpaRecognizerHolder();

    SherpaRecognizerHolder(const SherpaRecognizerHolder&) = delete;
    SherpaRecognizerHolder& operator=(const SherpaRecognizerHolder&) = delete;

    const SherpaOnnxOnlineRecognizer* Raw() const { return recognizer_; }
    const SherpaOnnxOnlineStream* CreateStream() const;
    void DestroyStream(const SherpaOnnxOnlineStream* stream) const;

    bool IsReady(const SherpaOnnxOnlineStream* stream);
    void Decode(const SherpaOnnxOnlineStream* stream);
    std::string GetResult(const SherpaOnnxOnlineStream* stream);
    void ResetStream(const SherpaOnnxOnlineStream* stream);

private:
    const SherpaOnnxOnlineRecognizer* recognizer_ = nullptr;
    std::mutex mutex_;
};
#endif

class SherpaOnnxAsrSession : public AsrSession {
public:
#ifdef HAVE_SHERPA_ONNX
    SherpaOnnxAsrSession(std::shared_ptr<SherpaRecognizerHolder> holder,
                         AsrSession::ErrorCallback errorCb,
                         uint64_t sessionId);
#else
    SherpaOnnxAsrSession(AsrSession::ErrorCallback errorCb,
                         uint64_t sessionId);
#endif
    ~SherpaOnnxAsrSession() override;

    void FeedAudio(const float* pcm, size_t frames) override;
    void End() override;
    void Cancel() override;
    void JoinWithTimeout(std::chrono::milliseconds timeout) override;
    void StartWorker() override;

private:
    void WorkerLoop();

#ifdef HAVE_SHERPA_ONNX
    std::shared_ptr<SherpaRecognizerHolder> holder_;
    const SherpaOnnxOnlineStream* stream_ = nullptr;
#endif

    std::shared_ptr<ThreadSafeQueue<std::vector<float>>> audioChunks_;
    std::unique_ptr<std::thread> workerThread_;
};

class SherpaOnnxAsrEngine : public AsrEngine {
public:
    SherpaOnnxAsrEngine();
    ~SherpaOnnxAsrEngine() override;

    bool Init(const Config& config) override;
    AsrSessionStart StartSession() override;
    const char* Name() const override { return "sherpa_onnx"; }

private:
    Config config_;
#ifdef HAVE_SHERPA_ONNX
    std::shared_ptr<SherpaRecognizerHolder> holder_;
#endif
};

} // namespace fcitx
