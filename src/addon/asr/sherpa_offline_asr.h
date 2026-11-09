#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
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
/// 持有 SherpaOnnxOfflineRecognizer，由 Engine 和所有 Session 共享。
///
/// 生命周期由 shared_ptr 管理，热重载时旧会话不会持有已销毁的 recognizer。
/// 不加锁：每次 Transcribe 使用独立 stream，recognizer 只读；onnxruntime 的
/// session 本身线程安全（与 sherpa-onnx 官方多线程解码示例一致）。
class SherpaOfflineRecognizerHolder {
public:
    explicit SherpaOfflineRecognizerHolder(
        const SherpaOnnxOfflineRecognizer* recognizer);
    ~SherpaOfflineRecognizerHolder();

    SherpaOfflineRecognizerHolder(const SherpaOfflineRecognizerHolder&) = delete;
    SherpaOfflineRecognizerHolder& operator=(
        const SherpaOfflineRecognizerHolder&) = delete;

    const SherpaOnnxOfflineRecognizer* Raw() const { return recognizer_; }

    /// 整段音频离线解码（Accept + Decode + GetResult），一次调用完成。
    /// 返回转写文本；失败、取消或空音频返回空串。
    std::string Transcribe(const float* pcm, size_t frames,
                           const std::atomic<bool>& cancelled);

private:
    const SherpaOnnxOfflineRecognizer* recognizer_ = nullptr;
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
    /// 等待 End()/Cancel()，整段解码后发出唯一的 final 结果。
    void WorkerLoop();
    void TranscribeUtterance(std::vector<float> pcm);

#ifdef HAVE_SHERPA_ONNX
    std::shared_ptr<SherpaOfflineRecognizerHolder> holder_;
#endif

    // FeedAudio/End 与 worker 之间的缓冲交接
    std::vector<float> pcmBuffer_;
    std::mutex bufferMutex_;
    std::condition_variable bufferCv_;
    bool endRequested_ = false;

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
