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

#ifdef HAVE_SHERPA_ONNX
#include <sherpa-onnx/c-api/c-api.h>
#endif

namespace fcitx {

#ifdef HAVE_SHERPA_ONNX
/// 线程安全封装 SherpaOnnxOfflineRecognizer 生命周期及并发解码操作。
/// 由 Engine 和所有 Session 共享持有，避免热重载时发生 UAF。
class SherpaOfflineRecognizerHolder {
public:
    explicit SherpaOfflineRecognizerHolder(const SherpaOnnxOfflineRecognizer* recognizer);
    ~SherpaOfflineRecognizerHolder();

    SherpaOfflineRecognizerHolder(const SherpaOfflineRecognizerHolder&) = delete;
    SherpaOfflineRecognizerHolder& operator=(const SherpaOfflineRecognizerHolder&) = delete;

    const SherpaOnnxOfflineRecognizer* Raw() const { return recognizer_; }
    const SherpaOnnxOfflineStream* CreateStream() const;
    void DestroyStream(const SherpaOnnxOfflineStream* stream) const;

    /// 整段音频离线解码（Accept + Decode + GetResult），一次调用完成。
    /// 返回转写文本；失败或取消返回空串。
    std::string Transcribe(const float* pcm, size_t frames,
                           const std::atomic<bool>& cancelled);

private:
    const SherpaOnnxOfflineRecognizer* recognizer_ = nullptr;
    mutable std::mutex mutex_;
};
#endif

class SherpaOfflineAsrSession : public AsrSession {
public:
#ifdef HAVE_SHERPA_ONNX
    SherpaOfflineAsrSession(std::shared_ptr<SherpaOfflineRecognizerHolder> holder,
                            AsrSession::ErrorCallback errorCb,
                            uint64_t sessionId);
#else
    SherpaOfflineAsrSession(AsrSession::ErrorCallback errorCb,
                            uint64_t sessionId);
#endif
    ~SherpaOfflineAsrSession() override;

    void FeedAudio(const float* pcm, size_t frames) override;
    void End() override;
    void Cancel() override;
    void JoinWithTimeout(std::chrono::milliseconds timeout) override;
    void StartWorker() override;

private:
    void TranscribeWorker(std::vector<float> pcm);

#ifdef HAVE_SHERPA_ONNX
    std::shared_ptr<SherpaOfflineRecognizerHolder> holder_;
#endif

    std::vector<float> pcmBuffer_;
    std::mutex bufferMutex_;
    std::unique_ptr<std::thread> workerThread_;
};

class SherpaOfflineAsrEngine : public AsrEngine {
public:
    SherpaOfflineAsrEngine();
    ~SherpaOfflineAsrEngine() override;

    bool Init(const Config& config) override;
    AsrSessionStart StartSession() override;
    const char* Name() const override { return "sherpa_offline"; }

private:
    Config config_;
#ifdef HAVE_SHERPA_ONNX
    std::shared_ptr<SherpaOfflineRecognizerHolder> holder_;
#endif
};

} // namespace fcitx
