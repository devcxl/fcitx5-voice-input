#include "pipeline.h"

#include <fcitx-utils/log.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "capture/pipewire_capture.h"
#include "capture/pulse_audio_capture.h"
#include "llm/llm_client.h"

using namespace std::chrono_literals;

namespace fcitx {

Pipeline::Pipeline()
    : vadWorker_(std::make_unique<VADWorker>()) {}

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
}

void Pipeline::SetAsrEngine(std::unique_ptr<AsrEngine> engine) {
    asrEngine_ = std::move(engine);
    if (asrEngine_) {
        asrEngine_->SetResultCallback(
            [this](const std::string& text, bool isFinal) {
                uint64_t gen = generation_.load();

                if (isFinal) {
                    if (text.empty()) return;

                    uint64_t uid = ++utteranceCounter_;

                    // Drain stale LLM-refined leftovers from previous utterance
                    AsrResult stale;
                    while (resultQueue_.TryPop(stale)) {
                        if (stale.isLLMRefined) {
                            FCITX_DEBUG() << "[voice-input] Drained stale LLM result: "
                                         << "uid=" << stale.utteranceId
                                         << " expect=" << uid;
                        }
                    }

                    // Push raw ASR result immediately
                    AsrResult rawResult;
                    rawResult.text = text;
                    rawResult.generation = gen;
                    rawResult.utteranceId = uid;
                    rawResult.isLLMRefined = false;
                    FCITX_INFO() << "[voice-input] ASR raw: uid=" << uid
                                 << " text=\"" << text << "\"";
                    resultQueue_.Push(std::move(rawResult));

                    if (resultCb_) {
                        resultCb_(text);
                    }

                    // If LLM is configured, process and push refined result
                    if (llmClient_) {
                        FCITX_DEBUG() << "[voice-input] LLM refine started"
                                      << " uid=" << uid << " gen=" << gen
                                      << " stream=" << llmStream_;
                        if (llmStream_) {
                            llmClient_->ProcessStream(text,
                                // onToken: push partial refined result
                                [this, uid, gen](const std::string& partial) {
                                    AsrResult partialResult;
                                    partialResult.text = partial;
                                    partialResult.generation = gen;
                                    partialResult.utteranceId = uid;
                                    partialResult.isLLMRefined = true;
                                    partialResult.isPartial = true;
                                    FCITX_DEBUG() << "[voice-input] LLM partial: uid="
                                                  << uid << " text=\"" << partial << "\"";
                                    resultQueue_.Push(std::move(partialResult));
                                    if (resultCb_) {
                                        resultCb_(partial);
                                    }
                                },
                                // onComplete: push final refined result
                                [this, uid, gen](const std::string& fullText) {
                                    AsrResult finalResult;
                                    finalResult.text = fullText;
                                    finalResult.generation = gen;
                                    finalResult.utteranceId = uid;
                                    finalResult.isLLMRefined = true;
                                    finalResult.isPartial = false;
                                    FCITX_INFO() << "[voice-input] LLM final: uid="
                                                 << uid << " text=\"" << fullText << "\"";
                                    resultQueue_.Push(std::move(finalResult));
                                    if (resultCb_) {
                                        resultCb_(fullText);
                                    }
                                });
                        } else {
                            std::string processed = llmClient_->Process(text);
                            if (!processed.empty()) {
                                AsrResult refinedResult;
                                refinedResult.text = processed;
                                refinedResult.generation = gen;
                                refinedResult.utteranceId = uid;
                                refinedResult.isLLMRefined = true;
                                FCITX_INFO() << "[voice-input] LLM refined push: uid="
                                             << uid << " text=\"" << processed << "\"";
                                resultQueue_.Push(std::move(refinedResult));
                                if (resultCb_) {
                                    resultCb_(processed);
                                }
                            } else {
                                FCITX_DEBUG() << "[voice-input] LLM refine skipped"
                                              << " (empty result) uid=" << uid;
                            }
                        }
                    }
                } else if (!text.empty()) {
                    // ASR partial result — push for preedit updates
                    AsrResult partial;
                    partial.text = text;
                    partial.generation = gen;
                    partial.isLLMRefined = false;
                    partial.isPartial = true;
                    resultQueue_.Push(std::move(partial));
                    if (resultCb_) {
                        resultCb_(text);
                    }
                }
            });
        asrEngine_->SetErrorCallback(
            [this](const std::string& error) {
                FCITX_ERROR() << "[voice-input] ASR error: " << error;
            });
    }
}

void Pipeline::SetResultCallback(ResultCallback cb) {
    resultCb_ = std::move(cb);
}

void Pipeline::SetVadStatusCallback(VADWorker::VadStatusCallback cb) {
    vadWorker_->SetVadStatusCallback(std::move(cb));
}

void Pipeline::Start() {
    if (running_) return;

    if (!asrEngine_) {
        FCITX_ERROR() << "[voice-input] No ASR engine configured";
        return;
    }

    if (!StartCapture()) return;

    // Drain stale results from previous session
    AsrResult stale;
    while (resultQueue_.TryPop(stale)) {}

    vadWorker_->Start();

    running_ = true;
    asrThread_ = std::make_unique<std::thread>(&Pipeline::AsrWorkerLoop, this);

    FCITX_INFO() << "[voice-input] Pipeline started";
}

void Pipeline::Stop() {
    if (!running_) return;

    running_ = false;

    if (capture_) {
        capture_->Stop();
    }

    vadWorker_->Stop();

    if (asrThread_ && asrThread_->joinable()) {
        asrThread_->join();
        asrThread_.reset();
    }

    if (asrEngine_) {
        asrEngine_->Stop();
    }

    if (capture_) {
        capture_.reset();
    }

    // Drain remaining results (commit via resultCb_ if still valid)
    AsrResult r;
    while (resultQueue_.TryPop(r)) {}

    FCITX_INFO() << "[voice-input] Pipeline stopped";
}

void Pipeline::Abort() {
    running_ = false;

    if (capture_) {
        capture_->Stop();
        capture_.reset();
    }

    vadWorker_->Stop();

    if (asrThread_ && asrThread_->joinable()) {
        asrThread_->join();
        asrThread_.reset();
    }

    if (asrEngine_) {
        asrEngine_->Stop();
    }

    // Clear queues
    AudioFrame f;
    while (frameQueue_.TryPop(f)) {}
    SpeechEvent se;
    while (speechEventQueue_.TryPop(se)) {}
    AsrResult r;
    while (resultQueue_.TryPop(r)) {}
}

bool Pipeline::StartCapture() {
    // PulseAudio first (also works with pipewire-pulse)
    capture_ = std::make_unique<PulseAudioCapture>();
    capture_->SetFrameQueue(&frameQueue_);

    if (capture_->Start()) {
        FCITX_INFO() << "[voice-input] Capture: " << capture_->Name();
        return true;
    }

    // Fallback to native PipeWire
    FCITX_WARN() << "[voice-input] PulseAudio failed, falling back to PipeWire";
    capture_ = std::make_unique<PipeWireCapture>();
    capture_->SetFrameQueue(&frameQueue_);

    if (capture_->Start()) {
        FCITX_INFO() << "[voice-input] Capture: " << capture_->Name();
        return true;
    }

    FCITX_ERROR() << "[voice-input] Failed to start any capture backend";
    capture_.reset();
    return false;
}

void Pipeline::AsrWorkerLoop() {
    static constexpr float kInt16ToFloat = 1.0f / 32768.0f;
    static constexpr int kChunkSamples = kSampleRate * 200 / 1000;

    while (running_) {
        SpeechEvent ev;
        if (!speechEventQueue_.TryPop(ev)) {
            std::this_thread::sleep_for(5ms);
            continue;
        }

        switch (ev.type) {
        case SpeechEventType::Begin:
            pendingAsrAudio_.clear();
            if (asrEngine_) {
                asrEngine_->Start();
                FCITX_DEBUG() << "[voice-input:asr] SpeechEvent Begin -> Start()";
            }
            break;

        case SpeechEventType::Audio:
            if (!asrEngine_ || ev.pcm.empty()) break;

            // Batch frames to ~200ms chunks
            pendingAsrAudio_.reserve(pendingAsrAudio_.size() + ev.pcm.size());
            for (int16_t sample : ev.pcm) {
                pendingAsrAudio_.push_back(static_cast<float>(sample) * kInt16ToFloat);
            }

            if (pendingAsrAudio_.size() >= static_cast<size_t>(kChunkSamples)) {
                asrEngine_->FeedAudio(pendingAsrAudio_.data(), pendingAsrAudio_.size());
                pendingAsrAudio_.clear();
            }
            break;

        case SpeechEventType::End:
            if (asrEngine_) {
                if (!pendingAsrAudio_.empty()) {
                    asrEngine_->FeedAudio(pendingAsrAudio_.data(), pendingAsrAudio_.size());
                    pendingAsrAudio_.clear();
                }
                asrEngine_->Stop();
                FCITX_DEBUG() << "[voice-input:asr] SpeechEvent End -> Stop()";
            }
            break;

        case SpeechEventType::Cancel:
            pendingAsrAudio_.clear();
            if (asrEngine_) {
                asrEngine_->Stop();
                FCITX_DEBUG() << "[voice-input:asr] SpeechEvent Cancel -> Stop()";
            }
            break;
        }
    }
}

} // namespace fcitx
