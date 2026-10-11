#include "mistral_asr.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <curl/curl.h>
#include <fcitx-utils/log.h>

#include "protocol/mistral_protocol.h"
#include "utils/asr_worker_boundary.h"
#include "utils/curl_resources.h"
#include "utils/ws_frame_receiver.h"
#include "utils/ws_frame_sender.h"

using namespace std::chrono_literals;

namespace fcitx {
namespace {

constexpr int kAppendChunkSamples = 1600; // ~100ms @16k
constexpr const char* kLogTag = "[voice-input:mistral]";
using namespace mistral_protocol;

std::string DescribeConnectFailure(CURLcode result, long status) {
    if (status != 0 && status != 101) {
        std::string message = "Mistral WS handshake failed: HTTP " + std::to_string(status);
        if (status == 401 || status == 403) message += " (check API key and permissions)";
        if (status == 404) message += " (check base URL and model)";
        if (status == 429) message += " (rate limited)";
        return message;
    }
    return "Mistral connect failed: " + std::string(curl_easy_strerror(result));
}

bool PermanentHttpFailure(long status) {
    return status >= 300 && status < 500 && status != 408 && status != 429;
}

void CloseWebSocket(CURL* curl) {
    size_t sent = 0;
    const auto result = curl_ws_send(curl, "", 0, &sent, 0, CURLWS_CLOSE);
    if (result != CURLE_OK)
        FCITX_WARN() << kLogTag << " WS close failed: " << curl_easy_strerror(result);
}

} // namespace

MistralAsrSession::MistralAsrSession(const AsrEngine::Config& config,
                                   AsrSession::ErrorCallback errorCb,
                                   uint64_t sessionId) {
    state_->sessionId = sessionId;
    errorCb_ = std::move(errorCb);
    audioChunks_ = std::make_shared<ThreadSafeQueue<std::vector<int16_t>>>(512);
    std::string base = config.apiEndpoint.empty() ? "https://api.mistral.ai/v1"
                                                 : config.apiEndpoint;
    if (base.starts_with("https://")) base.replace(0, 8, "wss://");
    else if (base.starts_with("http://")) base.replace(0, 7, "ws://");
    if (base.back() != '/') base += '/';
    modelName_ = config.modelName.empty() ? "voxtral-mini-transcribe-realtime-2602"
                                         : config.modelName;
    endpoint_ = base + "audio/transcriptions/realtime?model=" + modelName_;
    apiKey_ = config.apiKey;
    commitIntervalMs_ = std::clamp(config.commitIntervalMs, 1000, 30000);
    targetStreamingDelayMs_ = std::clamp(config.targetStreamingDelayMs, 0, 1000);
    // Constructor/start failure notification remains CRASH-ASR-002's scope.
    if (apiKey_.empty()) {
        state_->finished = true;
        if (errorCb_) errorCb_("Mistral API key not configured");
        if (resultCb_) resultCb_("", true, state_->sessionId);
    }
}

MistralAsrSession::~MistralAsrSession() {
    Cancel();
    JoinWithTimeout(5s);
}

void MistralAsrSession::StartWorker() {
    if (!state_->finished && !workerThread_) {
        auto self = std::static_pointer_cast<MistralAsrSession>(shared_from_this());
        state_->workerDone.store(false, std::memory_order_release);
        workerThread_ = std::make_unique<std::thread>([self]() {
            RunAsrWorker(*self->state_, kLogTag, self->errorCb_,
                         [self] { self->WorkerLoop(); },
                         [self] { self->PublishFinal({}); });
        });
    }
}

void MistralAsrSession::FeedAudio(const float* pcm, size_t frames) {
    if (state_->cancelled || state_->finished) return;
    if (!audioChunks_) return;
    std::vector<int16_t> chunk(frames);
    for (size_t i = 0; i < frames; ++i) {
        float s = std::clamp(pcm[i], -1.0f, 1.0f);
        chunk[i] = static_cast<int16_t>(s * 32767.0f);
    }
    audioChunks_->Push(std::move(chunk));
}

void MistralAsrSession::End() {
    endBudget_.Arm();
    state_->finished = true;
    if (audioChunks_) audioChunks_->Push(std::vector<int16_t>());
}

void MistralAsrSession::Cancel() {
    state_->cancelled = true;
    if (audioChunks_) audioChunks_->Push(std::vector<int16_t>());
}

void MistralAsrSession::JoinWithTimeout(std::chrono::milliseconds timeout) {
    if (workerThread_ && workerThread_->joinable()) {
        if (workerThread_->get_id() == std::this_thread::get_id()) {
            // The worker may own the last reference; never join itself.
            workerThread_->detach();
        } else if (WaitForWorkerCompletion(state_->workerDone, timeout)) {
            workerThread_->join();
        } else {
            FCITX_WARN() << kLogTag << " Join timeout session=" << state_->sessionId;
            workerThread_->detach();
        }
    }
    workerThread_.reset();
}

void MistralAsrSession::WorkerLoop() {
    const auto sid = state_->sessionId;
    auto reportError = [&](const std::string& message) {
        NotifyAsrError(errorCb_, message, kLogTag, sid);
    };
    auto fail = [&](const std::string& message, const std::string& fallback = std::string{}) {
        reportError(message);
        PublishFinal(fallback);
    };
    std::string previousTranscript;
    std::vector<int16_t> pending16k;
    std::string lastConnectFailure;

    for (int attempt = 0; attempt < kMaxReconnectAttempts; ++attempt) {
        if (state_->cancelled) return;
        if (state_->finished) {
            if (!lastConnectFailure.empty()) reportError(lastConnectFailure);
            PublishFinal(previousTranscript);
            return; // End must never start another connection.
        }
        CurlHeaders headers;
        CurlHandle connection(curl_easy_init());
        CURL* curl = connection.get();
        if (!curl) { fail("Failed to init curl", previousTranscript); return; }
        if (!headers.Append("Authorization: Bearer " + apiKey_)) {
            fail("Failed to allocate curl headers", previousTranscript);
            return;
        }
        curl_easy_setopt(curl, CURLOPT_URL, endpoint_.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.Get());
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "fcitx5-voice-input/0.1.0");
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        connectAbort_ = WsAbort::CancelOrFinished(state_->cancelled, state_->finished);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, WsAbortProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &connectAbort_);
        const auto connectResult = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (state_->cancelled) return;
        if (connectResult != CURLE_OK) {
            // End abort is control flow, not a replacement for an earlier HTTP
            // rejection. Never log URL, headers, or server response bodies.
            if (connectResult == CURLE_ABORTED_BY_CALLBACK && state_->finished &&
                status == 0 && lastConnectFailure.empty()) {
                // No HTTP response was observed. End is not a transport fault;
                // do not invent an "application callback" error as its cause.
                PublishFinal(previousTranscript);
                return;
            }
            if (status != 0 || connectResult != CURLE_ABORTED_BY_CALLBACK ||
                lastConnectFailure.empty())
                lastConnectFailure = DescribeConnectFailure(connectResult, status);
            FCITX_ERROR() << kLogTag << " " << lastConnectFailure
                          << " attempt=" << attempt << " session=" << sid;
            if (PermanentHttpFailure(status) || state_->finished ||
                attempt == kMaxReconnectAttempts - 1) {
                fail(lastConnectFailure, previousTranscript);
                return;
            }
            connection.reset();
            const auto retryAt = std::chrono::steady_clock::now() + 1s;
            while (!state_->cancelled && !state_->finished &&
                   std::chrono::steady_clock::now() < retryAt)
                std::this_thread::sleep_for(10ms);
            continue;
        }
        lastConnectFailure.clear();
        FCITX_INFO() << kLogTag << " WS connected session=" << sid;
        WsFrameReceiver receiver(CURLWS_TEXT);
        bool created = false;
        std::string segmentTranscript;
        std::string deltaTranscript;
        auto transcript = [&] { return previousTranscript + segmentTranscript + deltaTranscript; };
        auto publishPartial = [&] {
            const auto text = transcript();
            if (resultCb_ && !text.empty()) resultCb_(text, false, sid);
        };
        auto receive = [&](std::chrono::milliseconds timeout) -> bool {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!state_->cancelled && !finalPublished_ &&
                   !endBudget_.Expired() && std::chrono::steady_clock::now() < deadline) {
                const auto result = ReceiveWsMessage(curl, receiver, kLogTag);
                if (result == WsMessageStatus::Again) {
                    std::this_thread::sleep_for(5ms);
                    continue;
                }
                if (result == WsMessageStatus::Skipped) continue;
                if (result == WsMessageStatus::Closed || result == WsMessageStatus::Error) {
                    reportError("Mistral WebSocket disconnected before transcription.done");
                    PublishFinal(transcript());
                    return false;
                }
                ServerEvent event;
                if (!ParseServerEvent(receiver.AsString(), event)) {
                    fail("Invalid Mistral event");
                    return false;
                }
                switch (event.type) {
                case EventType::Created: created = true; return true;
                case EventType::Delta:
                    deltaTranscript += event.text;
                    publishPartial();
                    break;
                case EventType::Segment:
                    // Segment text confirms the deltas for that segment, not
                    // an additional copy. Done is authoritative per connection.
                    segmentTranscript += event.text;
                    deltaTranscript.clear();
                    publishPartial();
                    break;
                case EventType::Done:
                    PublishFinal(previousTranscript + event.text);
                    return true;
                case EventType::Error:
                    fail(event.error);
                    return false;
                case EventType::Ignored: break;
                }
            }
            return true;
        };
        // Match official SDK: await application-level creation before update
        // or audio. End during this wait still observes its total budget.
        const WsDeadline creationBudget;
        while (!created && !state_->cancelled && !finalPublished_ &&
               !creationBudget.Expired() && !endBudget_.Expired()) {
            if (!receive(20ms)) break;
        }
        if (state_->cancelled || finalPublished_) return;
        if (!created) {
            fail("Mistral session creation timed out", previousTranscript);
            return;
        }
        auto send = [&](const std::string& payload, bool ending = false) {
            return SendWsMessage(curl, payload, CURLWS_TEXT,
                                 ending ? WsAbort::CancelOnly(state_->cancelled)
                                        : WsAbort::CancelOrFinished(state_->cancelled, state_->finished),
                                 kLogTag, endBudget_.Deadline());
        };
        if (!send(BuildSessionUpdate(targetStreamingDelayMs_), true)) {
            fail("Mistral session update failed", previousTranscript);
            return;
        }
        const auto sessionStart = std::chrono::steady_clock::now();
        auto lastFlushTime = sessionStart;
        bool appendedSinceFlush = false;
        bool reconnectNeeded = false;
        while (!state_->cancelled && !finalPublished_) {
            if (endBudget_.Expired()) {
                reportError("Mistral End wait timed out");
                PublishFinal(transcript());
                break;
            }
            std::vector<int16_t> chunk;
            const bool hasChunk = audioChunks_->TryPop(chunk);
            if (hasChunk && chunk.empty() && state_->finished) {
                if ((!pending16k.empty() && !send(BuildAppendEvent(pending16k), true)) ||
                    !send(BuildControlEvent("input_audio.flush"), true) ||
                    !send(BuildControlEvent("input_audio.end"), true)) {
                    reportError("Mistral End send failed");
                    PublishFinal(transcript());
                    break;
                }
                while (!state_->cancelled && !finalPublished_ && !endBudget_.Expired())
                    if (!receive(20ms)) break;
                if (!state_->cancelled && !finalPublished_) {
                    reportError("Mistral End wait timed out");
                    PublishFinal(transcript());
                }
                break;
            }
            if (hasChunk) pending16k.insert(pending16k.end(), chunk.begin(), chunk.end());
            const auto now = std::chrono::steady_clock::now();
            if (appendedSinceFlush && now - lastFlushTime >= std::chrono::milliseconds(commitIntervalMs_)) {
                if (!send(BuildControlEvent("input_audio.flush"))) {
                    if (state_->finished || state_->cancelled) continue;
                    reconnectNeeded = true;
                    break;
                }
                lastFlushTime = now;
                appendedSinceFlush = false;
            }
            if (pending16k.size() >= kAppendChunkSamples) {
                if (!send(BuildAppendEvent(pending16k))) {
                    if (state_->finished || state_->cancelled) continue;
                    reconnectNeeded = true;
                    break;
                }
                pending16k.clear();
                appendedSinceFlush = true;
            }
            if (now - sessionStart >= kSessionMaxDuration && appendedSinceFlush) {
                if (!send(BuildControlEvent("input_audio.flush"))) {
                    if (state_->finished || state_->cancelled) continue;
                }
                reconnectNeeded = true;
                break;
            }
            if (!receive(20ms)) break;
            std::this_thread::sleep_for(5ms);
        }
        CloseWebSocket(curl);
        if (state_->cancelled || finalPublished_) return;
        if (!reconnectNeeded || state_->finished) {
            previousTranscript = transcript();
            break;
        }
        // The new server session has no in-flight segment context. Only retain
        // confirmed segments, not speculative deltas that could be replayed.
        previousTranscript += segmentTranscript;
    }
    if (!state_->cancelled) {
        reportError(lastConnectFailure.empty() ? "Mistral connection ended before completion"
                                               : lastConnectFailure);
        PublishFinal(previousTranscript);
    }
}

bool MistralAsrEngine::Init(const Config& config) {
    config_ = config;
    return true;
}

AsrSessionStart MistralAsrEngine::StartSession() {
    uint64_t sid;
    std::optional<uint64_t> cancelledSessionId;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        cancelledSessionId = CancelOldestSessionIfLimitReachedLocked();
        sid = NextSessionId();
    }
    auto session = std::make_shared<MistralAsrSession>(config_, errorCb_, sid);
    session->SetResultCallback(resultCb_);
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions_[sid] = session;
    }
    return {std::move(session), cancelledSessionId};
}

} // namespace fcitx
