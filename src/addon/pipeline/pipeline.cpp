#include "pipeline.h"

#include <fcitx-utils/log.h>
#include <algorithm>
#include <chrono>
#include <cmath>
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
    , reaper_(std::make_unique<SessionReaper>()) {}

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
    llmClient_ = std::move(client);
    if (llmClient_ && running_) {
        llmClient_->Activate(generation_.load());
    }
}

void Pipeline::SubmitOrderedResult(AsrResult result, bool terminal) {
    std::string notificationText;
    {
        std::lock_guard<std::mutex> lock(orderedResultMutex_);
        auto ready = orderedResults_.Submit(std::move(result), terminal);
        if (ready.empty()) return;

        notificationText = ready.back().text;
        for (auto& item : ready) {
            resultQueue_.Push(std::move(item));
        }
    }
    if (resultCb_) {
        resultCb_(notificationText);
    }
}

void Pipeline::SkipUtterance(uint64_t utteranceId) {
    std::string notificationText;
    {
        std::lock_guard<std::mutex> lock(orderedResultMutex_);
        auto ready = orderedResults_.Skip(utteranceId);
        if (ready.empty()) return;

        notificationText = ready.back().text;
        for (auto& item : ready) {
            resultQueue_.Push(std::move(item));
        }
    }
    if (resultCb_) {
        resultCb_(notificationText);
    }
}

void Pipeline::ResetOrderedResults() {
    std::lock_guard<std::mutex> lock(orderedResultMutex_);
    orderedResults_.Reset(utteranceCounter_.load() + 1);
}

void Pipeline::SetAsrEngine(std::unique_ptr<AsrEngine> engine) {
    std::shared_ptr<AsrEngine> enginePtr = std::move(engine);
    if (enginePtr) {
        enginePtr->SetResultCallback(
            [this, guard = resultGuard_](const std::string& text, bool isFinal,
                                         uint64_t sid) {
            // 会话 worker 线程可能晚于管线析构退出（reaper 超时 detach），
            // 回调前检查守卫，避免访问已析构的 Pipeline。
            if (!guard->load()) return;

            // Look up generation captured at session start
            SessionMetadata metadata;
            {
                std::lock_guard<std::mutex> lock(sessionMapMutex_);
                auto it = sessionGenerationMap_.find(sid);
                if (it == sessionGenerationMap_.end()) return;
                metadata = it->second;
                if (isFinal) {
                    sessionGenerationMap_.erase(sid);
                }
            }

            if (isFinal) {
                if (text.empty()) {
                    AsrResult errorResult;
                    errorResult.generation = metadata.generation;
                    errorResult.sessionId = sid;
                    errorResult.utteranceId = metadata.utteranceId;
                    errorResult.isError = true;
                    SubmitOrderedResult(std::move(errorResult), true);
                    return;
                }

                AsrResult rawResult;
                rawResult.text = text;
                rawResult.generation = metadata.generation;
                rawResult.sessionId = sid;
                rawResult.utteranceId = metadata.utteranceId;
                rawResult.isLLMRefined = false;
                FCITX_DEBUG() << "[voice-input] ASR raw: uid="
                             << metadata.utteranceId
                             << " text=\"" << text << "\"";
                SubmitOrderedResult(std::move(rawResult), !llmClient_);

                if (llmClient_) {
                    FCITX_DEBUG() << "[voice-input] LLM refine started"
                                 << " uid=" << metadata.utteranceId
                                 << " gen=" << metadata.generation
                                 << " stream=" << llmStream_;
                    if (llmStream_) {
                        llmClient_->ProcessStream(text, metadata.generation,
                            [this, guard, metadata,
                             sid](const std::string& partial) {
                                if (!guard->load()) return;
                                AsrResult partialResult;
                                partialResult.text = partial;
                                partialResult.generation = metadata.generation;
                                partialResult.sessionId = sid;
                                partialResult.utteranceId = metadata.utteranceId;
                                partialResult.isLLMRefined = true;
                                partialResult.isPartial = true;
                                SubmitOrderedResult(std::move(partialResult), false);
                            },
                            [this, guard, metadata,
                             sid](const std::string& fullText) {
                                if (!guard->load()) return;
                                AsrResult finalResult;
                                finalResult.text = fullText;
                                finalResult.generation = metadata.generation;
                                finalResult.sessionId = sid;
                                finalResult.utteranceId = metadata.utteranceId;
                                finalResult.isLLMRefined = true;
                                finalResult.isPartial = false;
                                SubmitOrderedResult(std::move(finalResult), true);
                            });
                    } else {
                        llmClient_->Process(
                            text, metadata.generation,
                            [this, guard, metadata,
                             sid](const std::string& processed) {
                                if (!guard->load()) return;
                                AsrResult refinedResult;
                                refinedResult.text = processed;
                                refinedResult.generation = metadata.generation;
                                refinedResult.sessionId = sid;
                                refinedResult.utteranceId = metadata.utteranceId;
                                refinedResult.isLLMRefined = true;
                                SubmitOrderedResult(std::move(refinedResult), true);
                            });
                    }
                }
            } else if (!text.empty()) {
                AsrResult partial;
                partial.text = text;
                partial.generation = metadata.generation;
                partial.sessionId = sid;
                partial.utteranceId = metadata.utteranceId;
                partial.isLLMRefined = false;
                partial.isPartial = true;
                SubmitOrderedResult(std::move(partial), false);
            }
            });
        enginePtr->SetErrorCallback(
            [this, guard = resultGuard_](const std::string& error) {
                if (!guard->load()) return;
                FCITX_ERROR() << "[voice-input] ASR error: " << error;
            });
    }

    std::shared_ptr<AsrEngine> previousEngine;
    std::vector<uint64_t> skippedUtterances;
    {
        // 与 Begin 的锁顺序一致：先 engineMutex_，后 sessionMapMutex_。
        std::lock_guard<std::mutex> engineLock(engineMutex_);
        previousEngine = std::move(asrEngine_);
        asrEngine_ = std::move(enginePtr);
        std::lock_guard<std::mutex> sessionLock(sessionMapMutex_);
        for (const auto& [sessionId, metadata] : sessionGenerationMap_) {
            skippedUtterances.push_back(metadata.utteranceId);
        }
        sessionGenerationMap_.clear();
    }
    if (previousEngine) {
        previousEngine->CancelAllSessions();
    }
    for (uint64_t utteranceId : skippedUtterances) {
        SkipUtterance(utteranceId);
    }
}

void Pipeline::SetResultCallback(ResultCallback cb) {
    resultCb_ = std::move(cb);
}

void Pipeline::SetVadStatusCallback(VADWorker::VadStatusCallback cb) {
    vadWorker_->SetVadStatusCallback(std::move(cb));
}

void Pipeline::Start() {
    if (running_) {
        if (llmClient_) llmClient_->Activate(generation_.load());
        return;
    }

    if (!asrEngine_) {
        FCITX_ERROR() << "[voice-input] No ASR engine configured";
        return;
    }

    if (!StartCapture()) return;

    // Drain stale results from previous session
    AsrResult stale;
    while (resultQueue_.TryPop(stale)) {}
    ResetOrderedResults();

    vadWorker_->Start();
    if (!vadWorker_->IsRunning()) {
        // VAD 初始化失败（如模型缺失）：回滚 capture，避免无消费者推帧
        FCITX_ERROR() << "[voice-input] VAD failed to start, aborting";
        capture_->Stop();
        capture_.reset();
        return;
    }

    if (llmClient_) llmClient_->Activate(generation_.load());
    running_ = true;
    asrThread_ = std::make_unique<std::thread>(&Pipeline::AsrDispatcherLoop, this);

    FCITX_INFO() << "[voice-input] Pipeline started";
}

void Pipeline::Stop() {
    if (!running_) return;

    running_ = false;
    if (llmClient_) {
        llmClient_->Cancel();
    }

    if (capture_) {
        capture_->Stop();
    }

    vadWorker_->Stop();

    if (asrThread_ && asrThread_->joinable()) {
        asrThread_->join();
        asrThread_.reset();
    }

    // Cancel active session and let reaper clean up in background
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

    if (capture_) {
        capture_.reset();
    }

    // Drain remaining results and audio queues（避免下次 Start 消费残留帧产生幽灵转写）
    AsrResult r;
    while (resultQueue_.TryPop(r)) {}
    AudioFrame f;
    while (frameQueue_.TryPop(f)) {}
    SpeechEvent se;
    while (speechEventQueue_.TryPop(se)) {}
    {
        std::lock_guard<std::mutex> lock(sessionMapMutex_);
        sessionGenerationMap_.clear();
    }
    ResetOrderedResults();

    FCITX_INFO() << "[voice-input] Pipeline stopped";
}

void Pipeline::Abort() {
    running_ = false;
    if (llmClient_) {
        llmClient_->Cancel();
    }

    if (capture_) {
        capture_->Stop();
        capture_.reset();
    }

    vadWorker_->Stop();

    if (asrThread_ && asrThread_->joinable()) {
        asrThread_->join();
        asrThread_.reset();
    }

    if (activeSession_) {
        activeSession_->Cancel();
        reaper_->Add(std::move(activeSession_));
        activeSessionId_ = 0;
    }

    std::shared_ptr<AsrEngine> engine;
    {
        std::lock_guard<std::mutex> lock(engineMutex_);
        engine = asrEngine_;
    }
    if (engine) {
        engine->CancelAllSessions();
    }

    // Clear queues
    AudioFrame f;
    while (frameQueue_.TryPop(f)) {}
    SpeechEvent se;
    while (speechEventQueue_.TryPop(se)) {}
    AsrResult r;
    while (resultQueue_.TryPop(r)) {}
    ResetOrderedResults();

    // 终态：回调守卫失效，引擎 worker 线程的异步回调全部丢弃
    resultGuard_->store(false);
}

bool Pipeline::StartCapture() {
#ifdef HAVE_PULSEAUDIO
    capture_ = std::make_unique<PulseAudioCapture>();
    capture_->SetFrameQueue(&frameQueue_);

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
    while (running_) {
        SpeechEvent ev;
        if (!speechEventQueue_.TryPop(ev)) {
            std::this_thread::sleep_for(5ms);
            continue;
        }

        switch (ev.type) {
        case SpeechEventType::Begin: {
            // 持锁到 session 映射建立与 worker 启动完成，避免热更新丢失终态。
            std::unique_lock<std::mutex> engineLock(engineMutex_);
            std::shared_ptr<AsrEngine> engine = asrEngine_;
            if (!engine) break;
            // Cancel current session if still active
            uint64_t cancelledUtteranceId = 0;
            if (activeSession_) {
                {
                    std::lock_guard<std::mutex> lock(sessionMapMutex_);
                    auto it = sessionGenerationMap_.find(activeSessionId_);
                    if (it != sessionGenerationMap_.end()) {
                        cancelledUtteranceId = it->second.utteranceId;
                        sessionGenerationMap_.erase(it);
                    }
                }
                activeSession_->Cancel();
                reaper_->Add(std::move(activeSession_));
                activeSessionId_ = 0;
            }
            if (cancelledUtteranceId != 0) {
                SkipUtterance(cancelledUtteranceId);
            }

            uint64_t limitCancelledUtteranceId = 0;
            uint64_t limitCancelledSessionId = 0;
            {
                std::lock_guard<std::mutex> lock(sessionMapMutex_);
                if (sessionGenerationMap_.size() >=
                    AsrEngine::kMaxActiveSessions) {
                    auto oldest = sessionGenerationMap_.begin();
                    for (auto it = sessionGenerationMap_.begin();
                         it != sessionGenerationMap_.end(); ++it) {
                        if (it->second.utteranceId < oldest->second.utteranceId) {
                            oldest = it;
                        }
                    }
                    limitCancelledSessionId = oldest->first;
                    limitCancelledUtteranceId = oldest->second.utteranceId;
                    sessionGenerationMap_.erase(oldest);
                }
            }
            if (limitCancelledUtteranceId != 0) {
                FCITX_WARN() << "[voice-input:asr] Too many sessions, skip uid="
                             << limitCancelledUtteranceId
                             << " session=" << limitCancelledSessionId;
                SkipUtterance(limitCancelledUtteranceId);
            }

            const uint64_t utteranceId = ++utteranceCounter_;
            // Start new session
            activeSession_ = engine->StartSession();
            if (activeSession_) {
                activeSessionId_ = activeSession_->GetState()->sessionId;
                if (activeSession_->GetState()->finished) {
                    FCITX_WARN() << "[voice-input:asr] Begin failed: session="
                                 << activeSessionId_ << " already finished";
                    activeSession_.reset();
                    activeSessionId_ = 0;
                    SkipUtterance(utteranceId);
                } else {
                    std::lock_guard<std::mutex> lock(sessionMapMutex_);
                    sessionGenerationMap_[activeSessionId_] = {
                        generation_.load(), utteranceId};
                    activeSession_->StartWorker();
                    FCITX_DEBUG() << "[voice-input:asr] Begin -> session="
                                 << activeSessionId_
                                 << " uid=" << utteranceId
                                 << " gen=" << generation_.load();
                }
            } else {
                FCITX_ERROR() << "[voice-input:asr] Begin failed: no ASR session";
                SkipUtterance(utteranceId);
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
            uint64_t cancelledUtteranceId = 0;
            if (activeSession_) {
                {
                    std::lock_guard<std::mutex> lock(sessionMapMutex_);
                    auto it = sessionGenerationMap_.find(activeSessionId_);
                    if (it != sessionGenerationMap_.end()) {
                        cancelledUtteranceId = it->second.utteranceId;
                        sessionGenerationMap_.erase(it);
                    }
                }
                activeSession_->Cancel();
                FCITX_DEBUG() << "[voice-input:asr] Cancel -> session="
                             << activeSessionId_;
                reaper_->Add(std::move(activeSession_));
                activeSessionId_ = 0;
            }
            if (cancelledUtteranceId != 0) {
                SkipUtterance(cancelledUtteranceId);
            }
            pendingAsrAudio_.clear();
            break;
        }
    }
}

} // namespace fcitx
