#include "pipeline.h"

#include <fcitx-utils/log.h>
#include <cstring>
#include <cmath>
#include <chrono>
#include <thread>
#include <memory>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "capture/pipewire_capture.h"
#include "capture/pulse_audio_capture.h"

using namespace std::chrono_literals;

namespace fcitx {

Pipeline::Pipeline()
    : vad_(std::make_unique<VAD>())
{
}

Pipeline::~Pipeline() {
    Abort();
    if (capture_) {
        capture_->Stop();
    }
}

void Pipeline::Init(const VoiceInputConfig& config) {
    config_ = config;

    VAD::Config vadConfig;
    vadConfig.threshold = static_cast<float>(config_.vadThreshold.value()) / 100.0f;
    vadConfig.silenceFrames = config_.silenceThresholdMs.value() / 20;
    vad_->SetConfig(vadConfig);

    FCITX_INFO() << "[voice-input] Init: vadThreshold=" << config_.vadThreshold.value()
                 << "% silenceThresholdMs=" << config_.silenceThresholdMs.value()
                 << " silenceFrames=" << vadConfig.silenceFrames;

    SetState(State::IDLE);
}

void Pipeline::StartListening() {
    if (state_.load() != State::IDLE) {
        FCITX_DEBUG() << "[voice-input] StartListening ignored: state=" << StateName();
        return;
    }

    FCITX_INFO() << "[voice-input] StartListening";

    if (!StartCapture()) {
        return;
    }

    asrCancelled_ = false;

    sessionAudio_.clear();
    sessionAudio_.reserve(kSessionReserveSamples);

    // Drain any stale audio from the ring buffer instead of Clear().
    float discard[320];
    int drained = 0;
    for (int i = 0; i < 1000; ++i) {
        if (!capture_ || capture_->RingBuffer()->Read(discard, 320) == 0) break;
        drained++;
    }
    if (drained > 0) {
        FCITX_INFO() << "[voice-input] Drained " << (drained * 320) << " stale samples from ring buffer";
    }

    vad_->Reset();

    SetState(State::LISTENING);

    captureThread_ = std::make_unique<std::thread>(&Pipeline::CaptureLoop, this);
}

void Pipeline::StopListening() {
    if (state_.load() == State::IDLE) return;

    FCITX_INFO() << "[voice-input] Pipeline::StopListening";

    asrCancelled_ = true;
    SetState(State::IDLE);

    if (captureThread_ && captureThread_->joinable()) {
        captureThread_->join();
        captureThread_.reset();
    }
    if (capture_) {
        capture_->Stop();
    }
    if (asrEngine_) {
        asrEngine_->Stop();
    }
}

void Pipeline::Abort() {
    asrCancelled_ = true;

    state_.store(State::IDLE);

    if (captureThread_ && captureThread_->joinable()) {
        captureThread_->join();
        captureThread_.reset();
    }
    if (capture_) {
        capture_->Stop();
    }

    if (asrEngine_) {
        asrEngine_->Stop();
    }
}

void Pipeline::SetConfig(const VoiceInputConfig& config) {
    config_ = config;
}

bool Pipeline::StartCapture() {
    if (capture_ && capture_->IsRunning()) {
        return true;
    }

    if (!capture_) {
        capture_ = std::make_unique<PulseAudioCapture>();
    }

    if (capture_->Start()) {
        FCITX_INFO() << "[voice-input] Using audio capture backend: " << capture_->Name();
        return true;
    }

    if (std::string(capture_->Name()) != "pipewire") {
        FCITX_WARN() << "[voice-input] Audio capture backend " << capture_->Name()
                     << " failed, falling back to pipewire";
        capture_ = std::make_unique<PipeWireCapture>();
        if (capture_->Start()) {
            FCITX_INFO() << "[voice-input] Using audio capture backend: " << capture_->Name();
            return true;
        }
    }

    FCITX_ERROR() << "[voice-input] Failed to start any audio capture backend";
    return false;
}

void Pipeline::SetAsrEngine(std::unique_ptr<AsrEngine> engine) {
    asrEngine_ = std::move(engine);
    if (asrEngine_) {
        asrEngine_->SetResultCallback(
            [this](const std::string& text, bool isFinal) {
                OnAsrResult(text, isFinal);
            });
    }
}

const char* Pipeline::StateName() const {
    switch (state_.load()) {
        case State::IDLE:            return "IDLE";
        case State::LISTENING:       return "LISTENING";
        case State::RECORDING:       return "RECORDING";
        case State::PROCESSING_ASR:  return "PROCESSING_ASR";
        case State::PROCESSING_LLM:  return "PROCESSING_LLM";
    }
    return "UNKNOWN";
}

void Pipeline::SetState(State newState) {
    State oldState = state_.exchange(newState);
    if (oldState != newState && stateCb_) {
        stateCb_(oldState, newState);
    }
}

void Pipeline::CaptureLoop() {
    constexpr size_t chunkFrames = 320;  // 20ms at 16kHz
    float chunk[chunkFrames];
    int emptyReadCount = 0;
    int discardCount = 0;

    while (state_.load() != State::IDLE) {
        size_t read = capture_ ? capture_->RingBuffer()->Read(chunk, chunkFrames) : 0;
        if (read == 0) {
            ++emptyReadCount;
            if (emptyReadCount % 400 == 0) {
                FCITX_WARN() << "[voice-input] No audio samples read from PipeWire ring buffer for ~2s"
                             << " (backend=" << (capture_ ? capture_->Name() : "none")
                             << " running=" << (capture_ && capture_->IsRunning()) << ")";
            }
            std::this_thread::sleep_for(5ms);
            continue;
        }
        emptyReadCount = 0;

        State curState = state_.load();

        if (curState == State::LISTENING) {
            vad_->Process(chunk, read);
            if (vad_->IsSpeechActive()) {
                float energy = 0.0f;
                for (size_t i = 0; i < read; ++i) energy += std::abs(chunk[i]);
                FCITX_INFO() << "[voice-input] VAD speech onset, energy="
                             << (energy / read);
                sessionAudio_.clear();
                sessionAudio_.insert(sessionAudio_.end(), chunk, chunk + read);
                SetState(State::RECORDING);
            }
        } else if (curState == State::RECORDING) {
            vad_->Process(chunk, read);
            sessionAudio_.insert(sessionAudio_.end(), chunk, chunk + read);
            if (!vad_->IsSpeechActive() && vad_->IsSilenceTimeout()) {
                float durSec = sessionAudio_.size() / 16000.0f;
                FCITX_INFO() << "[voice-input] VAD silence timeout, audio="
                             << sessionAudio_.size() << " frames ("
                             << durSec << "s), submitting";
                SetState(State::PROCESSING_ASR);
                DispatchToAsr();
            } else if (sessionAudio_.size() >= kMaxSessionSamples) {
                FCITX_INFO() << "[voice-input] Max recording duration reached, audio="
                             << sessionAudio_.size() << " frames, submitting";
                SetState(State::PROCESSING_ASR);
                DispatchToAsr();
            }
        }
        // PROCESSING_ASR / PROCESSING_LLM: discard audio, wait for state change
        if (curState == State::PROCESSING_ASR || curState == State::PROCESSING_LLM) {
            discardCount++;
            if (discardCount % 100 == 0) {
                FCITX_DEBUG() << "[voice-input] Discarding " << (discardCount * read)
                              << " samples during " << StateName();
            }
        }
    }
    FCITX_INFO() << "[voice-input] CaptureLoop exited (state=" << StateName() << ")";
}

void Pipeline::DispatchToAsr() {
    if (!asrEngine_) {
        FCITX_ERROR() << "[voice-input] No ASR engine configured";
        SetState(State::LISTENING);
        return;
    }

    if (sessionAudio_.empty()) {
        FCITX_WARN() << "[voice-input] No audio to process";
        SetState(State::LISTENING);
        return;
    }

    float durSec = sessionAudio_.size() / 16000.0f;
    FCITX_INFO() << "[voice-input] Dispatching " << sessionAudio_.size()
                 << " frames (" << durSec << "s) to ASR engine: "
                 << (asrEngine_ ? asrEngine_->Name() : "none");

    asrEngine_->Start();
    asrEngine_->FeedAudio(sessionAudio_.data(), sessionAudio_.size());
    sessionAudio_.clear();
    asrEngine_->Stop();
}

void Pipeline::OnAsrResult(const std::string& text, bool isFinal) {
    if (asrCancelled_) return;

    if (isFinal) {
        if (!text.empty()) {
            FCITX_INFO() << "[voice-input] ASR result: \"" << text << "\"";
        } else {
            FCITX_INFO() << "[voice-input] ASR result: (empty)";
        }

        if (!text.empty() && !config_.llmModel.value().empty()) {
            PostProcessWithLLM(text);
        } else if (!text.empty() && resultCb_) {
            resultCb_(text);
        }
        SetState(State::LISTENING);
    }
}

void Pipeline::PostProcessWithLLM(const std::string& text) {
    // Wait for previous LLM worker to finish
    if (llmThread_ && llmThread_->joinable()) {
        llmThread_->join();
    }

    llmThread_ = std::make_unique<std::thread>([this, text]() {
        std::string systemPrompt = config_.llmSystemPrompt.value();
        if (systemPrompt.empty()) {
            systemPrompt = "你是一个中文语音识别后处理助手。请修正以下语音识别结果中的错别字，保持原意，只返回修正后的纯文本。";
        }

        std::string corrected = DoLlmHttpRequest(text, systemPrompt);

        if (corrected.empty()) {
            FCITX_WARN() << "[voice-input:llm] Correction returned empty, using original";
            if (resultCb_) resultCb_(text);
        } else {
            FCITX_INFO() << "[voice-input:llm] Corrected: \"" << corrected << "\"";
            if (resultCb_) resultCb_(corrected);
        }
    });
}

std::string Pipeline::DoLlmHttpRequest(const std::string& userText,
                                        const std::string& systemPrompt) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string endpoint = config_.openaiEndpoint.value();
    if (endpoint.empty()) endpoint = "https://api.openai.com/v1";
    if (endpoint.back() != '/') endpoint += '/';
    std::string url = endpoint + "chat/completions";

    std::string model = config_.llmModel.value();
    FCITX_INFO() << "[voice-input:llm] POST " << url << " model=" << model;

    nlohmann::json body;
    body["model"] = model;
    body["messages"] = {
        {{"role", "system"}, {"content", systemPrompt}},
        {{"role", "user"}, {"content", userText}}
    };
    body["temperature"] = 0.0;
    std::string bodyStr = body.dump();

    struct curl_slist* headers = nullptr;
    std::string auth = "Authorization: Bearer " + config_.openaiApiKey.value();
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)bodyStr.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
        size_t total = size * nmemb;
        static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
        return total;
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "fcitx5-voice-input/0.1.0");

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        FCITX_ERROR() << "[voice-input:llm] HTTP error: " << curl_easy_strerror(res);
        return "";
    }

    FCITX_INFO() << "[voice-input:llm] Response: " << response.size() << " bytes";

    try {
        auto json = nlohmann::json::parse(response);
        if (json.contains("error")) {
            FCITX_ERROR() << "[voice-input:llm] API error: "
                          << json["error"].value("message", "unknown");
            return "";
        }
        std::string content = json["choices"][0]["message"]["content"];
        return content;
    } catch (const std::exception& e) {
        FCITX_ERROR() << "[voice-input:llm] JSON parse error: " << e.what();
        return "";
    }
}

} // namespace fcitx
