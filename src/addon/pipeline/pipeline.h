#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config/voiceinput-config.h"
#include "capture/audio_capture.h"
#include "vad/vad.h"
#include "asr/asr_engine.h"
#include "asr/asr_session.h"
#include "asr/session_reaper.h"
#include "pipeline/result_coordinator.h"
#include "types.h"
#include "utils/thread_safe_queue.h"

namespace fcitx {

class Pipeline {
public:
    using ResultCallback = std::function<void(const std::string& text)>;

    Pipeline();
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void Init(const VoiceInputConfig& config);
    void SetAsrEngine(std::unique_ptr<AsrEngine> engine);
    void SetLLMClient(std::unique_ptr<LLMClient> client);
    void SetLLMStream(bool stream) { results_->SetLLMStream(stream); }
    void SetResultCallback(ResultCallback cb);
    void SetVadStatusCallback(VADWorker::VadStatusCallback cb);
    void SetGeneration(uint64_t gen) { generation_.store(gen); }

    void Start();
    void Stop();
    void Abort();
    bool IsRunning() const { return running_.load(); }

    ThreadSafeQueue<AsrResult>& ResultQueue() {
        return results_->ResultQueue();
    }

    void SetConfig(const VoiceInputConfig& config);

private:
    bool StartCapture();
    void AsrDispatcherLoop();

    // 音频与语音事件可丢旧；识别终态由 ResultCoordinator 无损保存。
    // 1024 帧 ≈ 32s 音频缓冲；256 事件 ≈ 256 段语音。
    ThreadSafeQueue<AudioFrame> frameQueue_{1024};
    ThreadSafeQueue<SpeechEvent> speechEventQueue_{256};

    // Workers
    std::unique_ptr<VADWorker> vadWorker_;
    std::unique_ptr<std::thread> asrThread_;

    // Capture
    std::unique_ptr<AudioCapture> capture_;

    // ASR session management
    std::shared_ptr<AsrEngine> asrEngine_;
    std::shared_ptr<AsrSession> activeSession_;
    uint64_t activeSessionId_{0};
    std::mutex engineMutex_;       // 保护 asrEngine_ 指针替换与使用
    std::unique_ptr<SessionReaper> reaper_;

    // ASR streaming batching
    std::vector<float> pendingAsrAudio_;

    // State
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> utteranceCounter_{0};
    std::shared_ptr<ResultCoordinator> results_;

    // Config
    VoiceInputConfig config_;
};

} // namespace fcitx
