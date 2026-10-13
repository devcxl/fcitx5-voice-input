#include "pipeline.h"

#include <fcitx-utils/log.h>
#include <chrono>
#include <thread>

#ifdef HAVE_PIPEWIRE
#include "capture/pipewire_capture.h"
#endif
#ifdef HAVE_PULSEAUDIO
#include "capture/pulse_audio_capture.h"
#endif
#include "llm/llm_client.h"

using namespace std::chrono_literals;

namespace fcitx {

Pipeline::Pipeline()
    : vadWorker_(std::make_unique<VADWorker>())
    , reaper_(std::make_unique<SessionReaper>())
    , results_(std::make_shared<ResultCoordinator>()) {}

Pipeline::~Pipeline() {
    Abort();
}

void Pipeline::Init(const VoiceInputConfig& config) {
    config_ = config;

    VADWorker::Config vadConfig;
    vadConfig.speechThreshold =
        static_cast<float>(config_.vadThreshold.value()) / 100.0f;
    vadConfig.silenceThreshold =
        vadConfig.speechThreshold * 0.7f;
    vadConfig.endSilenceMs = config_.silenceThresholdMs.value();
    vadConfig.startFrames = config_.startFrames.value();
    vadConfig.preRollMs = config_.preRollMs.value();
    vadConfig.minSpeechMs = config_.minSpeechMs.value();
    vadConfig.maxSpeechMs = config_.maxSpeechMs.value();

    FCITX_INFO() << "[voice-input] Init:"
                 << " vadThreshold=" << config_.vadThreshold.value()
                 << "% silenceThresholdMs=" << config_.silenceThresholdMs.value()
                 << " startFrames=" << config_.startFrames.value()
                 << " preRollMs=" << config_.preRollMs.value()
                 << " minSpeechMs=" << config_.minSpeechMs.value()
                 << " maxSpeechMs=" << config_.maxSpeechMs.value();

    vadWorker_->SetConfig(vadConfig);
    vadWorker_->SetFrameQueue(&frameQueue_);
    vadWorker_->SetSpeechEventQueue(&speechEventQueue_);
}

void Pipeline::SetConfig(const VoiceInputConfig& config) {
    config_ = config;

    VADWorker::Config vadConfig;
    vadConfig.speechThreshold =
        static_cast<float>(config_.vadThreshold.value()) / 100.0f;
    vadConfig.silenceThreshold =
        vadConfig.speechThreshold * 0.7f;
    vadConfig.endSilenceMs = config_.silenceThresholdMs.value();
    vadConfig.startFrames = config_.startFrames.value();
    vadConfig.preRollMs = config_.preRollMs.value();
    vadConfig.minSpeechMs = config_.minSpeechMs.value();
    vadConfig.maxSpeechMs = config_.maxSpeechMs.value();
    vadWorker_->SetConfig(vadConfig);
}

void Pipeline::SetLLMClient(std::unique_ptr<LLMClient> client) {
    results_->SetLLMClient(std::shared_ptr<LLMClient>(std::move(client)));
}

void Pipeline::SetAsrEngine(std::unique_ptr<AsrEngine> engine) {
    std::shared_ptr<AsrEngine> enginePtr = std::move(engine);
    if (enginePtr) {
        auto results = results_;
        enginePtr->SetResultCallback(
            [results](const std::string& text, bool isFinal, uint64_t sid) {
                results->HandleAsrResult(text, isFinal, sid);
            });
        enginePtr->SetErrorCallback(
            [](const std::string& error) {
                FCITX_ERROR() << "[voice-input] ASR error: " << error;
            });
    }

    std::shared_ptr<AsrEngine> previousEngine;
    {
        std::lock_guard<std::mutex> lock(engineMutex_);
        results_->SkipAllSessions();
        previousEngine = std::move(asrEngine_);
        asrEngine_ = std::move(enginePtr);
    }
    if (previousEngine) previousEngine->CancelAllSessions();
}

void Pipeline::SetResultCallback(ResultCallback cb) {
    results_->SetResultCallback(std::move(cb));
}

void Pipeline::SetVadStatusCallback(VADWorker::VadStatusCallback cb) {
    vadWorker_->SetVadStatusCallback(std::move(cb));
}

void Pipeline::Start() {
    if (running_) {
        results_->Start(utteranceCounter_.load() + 1, generation_.load());
        return;
    }

    if (!asrEngine_) {
        FCITX_ERROR() << "[voice-input] No ASR engine configured";
        return;
    }

    // A retained stream may have died while the input context was unfocused.
    if (capture_ && !capture_->IsRunning()) {
        capture_->Stop();
        capture_.reset();
    }
    if (!capture_ && !StartCapture()) return;
    frameQueue_.Clear();
    speechEventQueue_.Clear();

    results_->Start(utteranceCounter_.load() + 1, generation_.load());

    vadWorker_->Start();
    if (!vadWorker_->IsRunning()) {
        // VAD 初始化失败（如模型缺失）：回滚 capture，避免无消费者推帧
        FCITX_ERROR() << "[voice-input] VAD failed to start, aborting";
        Stop();
        return;
    }

    running_ = true;
    asrThread_ = std::make_unique<std::thread>(&Pipeline::AsrDispatcherLoop, this);
    capture_->SetDeliveryEnabled(true);

    FCITX_INFO() << "[voice-input] Pipeline started";
}

bool Pipeline::IsCapturing() const {
    return capture_ && capture_->IsRunning();
}

void Pipeline::Pause() {
    // Close the frame gate first. An in-flight read cannot enqueue after this
    // returns, even across a rapid pause/resume (capture delivery epoch).
    if (capture_) capture_->SetDeliveryEnabled(false);
    const bool wasRunning = running_.exchange(false);
    results_->Pause(utteranceCounter_.load() + 1);
    vadWorker_->Stop();

    if (asrThread_ && asrThread_->joinable()) {
        asrThread_->join();
        asrThread_.reset();
    }

    // Dispatcher ownership ends at join. Do not race FeedAudio/End with the
    // control thread's cancellation or pending audio cleanup.
    if (activeSession_) {
        activeSession_->Cancel();
        reaper_->Add(std::move(activeSession_));
        activeSessionId_ = 0;
    }

    // Cancel all engine sessions (old ones will be cleaned by reaper)
    std::shared_ptr<AsrEngine> engine;
    {
        std::lock_guard<std::mutex> lock(engineMutex_);
        engine = asrEngine_;
    }
    if (engine) {
        engine->CancelAllSessions();
    }

    pendingAsrAudio_.clear();
    frameQueue_.Clear();
    speechEventQueue_.Clear();
    if (wasRunning) FCITX_INFO() << "[voice-input] Recognition paused; capture retained";
}

void Pipeline::Stop() {
    Pause();
    if (capture_) {
        capture_->Stop();
        capture_.reset();
    }
}

void Pipeline::Abort() {
    results_->Close(utteranceCounter_.load() + 1);
    Stop();
}

bool Pipeline::StartCapture() {
#ifdef HAVE_PULSEAUDIO
    capture_ = std::make_unique<PulseAudioCapture>();
    capture_->SetFrameQueue(&frameQueue_);
    capture_->SetDeliveryEnabled(false);

    if (capture_->Start()) {
        FCITX_INFO() << "[voice-input] Capture: " << capture_->Name();
        return true;
    }

    FCITX_WARN() << "[voice-input] PulseAudio failed, falling back to PipeWire";
    capture_.reset();
#endif

#ifdef HAVE_PIPEWIRE
    capture_ = std::make_unique<PipeWireCapture>();
    capture_->SetFrameQueue(&frameQueue_);
    capture_->SetDeliveryEnabled(false);

    if (capture_->Start()) {
        FCITX_INFO() << "[voice-input] Capture: " << capture_->Name();
        return true;
    }

    FCITX_ERROR() << "[voice-input] PipeWire failed to start";
    capture_.reset();
#endif

#if !defined(HAVE_PULSEAUDIO) && !defined(HAVE_PIPEWIRE)
    // CMake 已保证至少编译一个后端，此处仅作防御
    FCITX_ERROR() << "[voice-input] No capture backend compiled in";
#endif
    return false;
}

void Pipeline::AsrDispatcherLoop() {
    auto lastStaleCheck = std::chrono::steady_clock::now();
    while (running_) {
        SpeechEvent ev;
        if (!speechEventQueue_.TryPop(ev)) {
            std::this_thread::sleep_for(5ms);
            // 保序闸门兜底：被阻塞的语音段此后可能不再有任何回调，因此需要定时
            // 检查停滞并放行，避免单段故障把后续所有结果永久扣住（issue #43）。
            auto now = std::chrono::steady_clock::now();
            if (now - lastStaleCheck >= 500ms) {
                lastStaleCheck = now;
                results_->ExpireStale();
            }
            continue;
        }

        if (!running_) break;
        switch (ev.type) {
        case SpeechEventType::Begin: {
            // 持锁到 session 映射建立与 worker 启动完成，避免热更新丢失终态。
            std::unique_lock<std::mutex> engineLock(engineMutex_);
            if (!running_) break;
            std::shared_ptr<AsrEngine> engine = asrEngine_;
            if (!engine) break;
            // Cancel current session if still active
            if (activeSession_) {
                results_->SkipSession(activeSessionId_);
                activeSession_->Cancel();
                reaper_->Add(std::move(activeSession_));
                activeSessionId_ = 0;
            }

            const uint64_t utteranceId = ++utteranceCounter_;
            // Start new session
            auto start = engine->StartSession();
            if (start.cancelledSessionId) {
                FCITX_WARN() << "[voice-input:asr] Too many sessions, skip "
                             << "session=" << *start.cancelledSessionId;
                results_->SkipSession(*start.cancelledSessionId);
            }
            activeSession_ = std::move(start.session);
            if (activeSession_) {
                activeSessionId_ = activeSession_->GetState()->sessionId;
                if (activeSession_->GetState()->finished) {
                    FCITX_WARN() << "[voice-input:asr] Begin failed: session="
                                 << activeSessionId_ << " already finished";
                    activeSession_.reset();
                    activeSessionId_ = 0;
                    results_->SkipUtterance(utteranceId);
                } else {
                    results_->RegisterSession(
                        activeSessionId_, {generation_.load(), utteranceId});
                    activeSession_->StartWorker();
                    FCITX_DEBUG() << "[voice-input:asr] Begin -> session="
                                 << activeSessionId_
                                 << " uid=" << utteranceId
                                 << " gen=" << generation_.load();
                }
            } else {
                FCITX_ERROR() << "[voice-input:asr] Begin failed: no ASR session";
                results_->SkipUtterance(utteranceId);
            }
            pendingAsrAudio_.clear();
            break;
        }

        case SpeechEventType::Audio:
            if (!activeSession_ || ev.pcm.empty()) break;

            pendingAsrAudio_.reserve(pendingAsrAudio_.size() + ev.pcm.size());
            for (int16_t sample : ev.pcm) {
                pendingAsrAudio_.push_back(
                    static_cast<float>(sample) * (1.0f / 32768.0f));
            }

            // Batch to ~200ms chunks
            static constexpr int kChunkSamples = kSampleRate * 200 / 1000;
            if (pendingAsrAudio_.size() >= static_cast<size_t>(kChunkSamples)) {
                activeSession_->FeedAudio(
                    pendingAsrAudio_.data(), pendingAsrAudio_.size());
                pendingAsrAudio_.clear();
            }
            break;

        case SpeechEventType::End:
            if (activeSession_) {
                if (!pendingAsrAudio_.empty()) {
                    activeSession_->FeedAudio(
                        pendingAsrAudio_.data(), pendingAsrAudio_.size());
                    pendingAsrAudio_.clear();
                }
                activeSession_->End();
                FCITX_DEBUG() << "[voice-input:asr] End -> session="
                             << activeSessionId_;
                reaper_->Add(std::move(activeSession_));
                activeSessionId_ = 0;
            }
            break;

        case SpeechEventType::Cancel:
            if (activeSession_) {
                results_->SkipSession(activeSessionId_);
                activeSession_->Cancel();
                FCITX_DEBUG() << "[voice-input:asr] Cancel -> session="
                             << activeSessionId_;
                reaper_->Add(std::move(activeSession_));
                activeSessionId_ = 0;
            }
            pendingAsrAudio_.clear();
            break;
        }
    }
}

} // namespace fcitx
