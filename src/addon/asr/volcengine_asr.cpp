#include "volcengine_asr.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <curl/curl.h>
#include <fcitx-utils/log.h>
#include <json/json.h>

#include "protocol/volcengine_protocol.h"
#include "utils/ws_frame_receiver.h"
#include "utils/ws_frame_sender.h"

using namespace std::chrono_literals;

namespace fcitx {
namespace {

constexpr int kVolcengineSampleRate = 16000;
constexpr int kVolcengineBigmodelSuccessCode = 20000000;
constexpr int kVolcengineLegacySuccessCode = 1000;
constexpr const char* kLogTag = "[voice-input:volcengine]";

using namespace volcengine_protocol;

enum class RecvStatus { Ok, Again, Skipped, Closed, Error };

// 发送一帧二进制 WS 消息；上游不读 socket 时有界失败（issue #42）。
bool SendWebSocketBinary(CURL* curl, const uint8_t* data, size_t length,
                         const WsAbort& abort,
                         WsDeadline deadline = WsDeadline{}) {
    return SendWsMessage(curl, data, length, CURLWS_BINARY, abort, kLogTag,
                         deadline);
}

RecvStatus ReceiveWebSocketFrame(CURL* curl, WsFrameReceiver& receiver,
                                std::vector<uint8_t>& frame) {
    const WsMessageStatus r = ReceiveWsMessage(curl, receiver, kLogTag);
    if (r == WsMessageStatus::Ok) {
        frame = receiver.Bytes();
        return RecvStatus::Ok;
    }
    if (r == WsMessageStatus::Skipped) {
        frame.clear();
        return RecvStatus::Skipped;
    }
    if (r == WsMessageStatus::Again) return RecvStatus::Again;
    if (r == WsMessageStatus::Closed) return RecvStatus::Closed;
    return RecvStatus::Error;
}

void CloseWebSocket(CURL* curl) {
    if (!curl) return;
    size_t sent = 0;
    CURLcode res = curl_ws_send(curl, "", 0, &sent, 0, CURLWS_CLOSE);
    if (res != CURLE_OK)
        FCITX_WARN() << "[voice-input:volcengine] WS close failed: " << curl_easy_strerror(res);
}

} // namespace

// ── VolcengineAsrSession ──────────────────────────────────────

VolcengineAsrSession::VolcengineAsrSession(const AsrEngine::Config& config,
                                           AsrSession::ErrorCallback errorCb,
                                           uint64_t sessionId) {
    state_->sessionId = sessionId;
    errorCb_ = std::move(errorCb);
    audioChunks_ = std::make_shared<ThreadSafeQueue<std::vector<int16_t>>>(512);

    endpoint_ = config.apiEndpoint.empty()
                    ? "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
                    : config.apiEndpoint;
    authMode_ = config.authMode.empty() ? "api_key" : config.authMode;
    apiKey_ = config.apiKey;
    appKey_ = config.appKey;
    accessKey_ = config.accessKey;
    resourceId_ = config.resourceId.empty() ? "volc.seedasr.sauc.duration" : config.resourceId;
    modelName_ = config.modelName.empty() ? "bigmodel" : config.modelName;
    chunkMs_ = std::clamp(config.chunkMs, 100, 200);
    enableItN_ = config.enableItN;
    enablePunc_ = config.enablePunc;
    enableDdc_ = config.enableDdc;
    enableNonstream_ = config.enableNonstream;
    endWindowMs_ = std::clamp(config.endWindowMs, 200, 3000);

    bool valid = true;
    if (authMode_ == "api_key") {
        if (apiKey_.empty()) { valid = false; }
    } else if (authMode_ == "app_access_key") {
        if (appKey_.empty() || accessKey_.empty()) { valid = false; }
    }

    if (!valid) {
        state_->finished = true;
        if (errorCb_) errorCb_("Volcengine credentials not configured");
        if (resultCb_) resultCb_("", true, state_->sessionId);
        return;
    }

    FCITX_DEBUG() << "[voice-input:volcengine] Init session=" << sessionId
                 << " endpoint=" << endpoint_
                 << " apiKey=" << MaskSecret(apiKey_)
                 << " appKey=" << MaskSecret(appKey_)
                 << " accessKey=" << MaskSecret(accessKey_);
}

VolcengineAsrSession::~VolcengineAsrSession() {
    Cancel();
    JoinWithTimeout(5s);
}

void VolcengineAsrSession::StartWorker() {
    if (!state_->finished && !workerThread_) {
        auto self = std::static_pointer_cast<VolcengineAsrSession>(shared_from_this());
        state_->workerDone.store(false, std::memory_order_release);
        workerThread_ = std::make_unique<std::thread>([self]() {
            self->WorkerLoop();
            self->state_->workerDone.store(true, std::memory_order_release);
        });
    }
}

void VolcengineAsrSession::FeedAudio(const float* pcm, size_t frames) {
    if (state_->cancelled || state_->finished) return;
    if (!audioChunks_) return;
    std::vector<int16_t> chunk(frames);
    for (size_t i = 0; i < frames; ++i) {
        float s = std::clamp(pcm[i], -1.0f, 1.0f);
        chunk[i] = static_cast<int16_t>(s * 32767.0f);
    }
    audioChunks_->Push(std::move(chunk));
}

void VolcengineAsrSession::End() {
    endBudget_.Arm();
    state_->finished = true;
    if (audioChunks_) audioChunks_->Push(std::vector<int16_t>());
}

void VolcengineAsrSession::Cancel() {
    state_->cancelled = true;
    if (audioChunks_) audioChunks_->Push(std::vector<int16_t>());
}

void VolcengineAsrSession::JoinWithTimeout(std::chrono::milliseconds timeout) {
    if (workerThread_ && workerThread_->joinable()) {
        if (workerThread_->get_id() == std::this_thread::get_id()) {
            // worker 持有最后一个会话引用时，析构会在自身线程执行，不能 join 自身。
            workerThread_->detach();
        } else if (WaitForWorkerCompletion(state_->workerDone, timeout)) {
            workerThread_->join();
        } else {
            FCITX_WARN() << "[voice-input:volcengine] Join timeout session="
                         << state_->sessionId;
            workerThread_->detach();
        }
    }
    workerThread_.reset();
}

void VolcengineAsrSession::WorkerLoop() {
    uint64_t sid = state_->sessionId;
    auto audioChunks = audioChunks_;  // keep queue alive
    auto cb = resultCb_;
    auto ecb = errorCb_;

    const size_t chunkSamples = static_cast<size_t>(kVolcengineSampleRate * chunkMs_ / 1000);

    CURL* curl = curl_easy_init();
    if (!curl) {
        if (ecb) ecb("Failed to init curl");
        if (cb) cb("", true, sid);
        return;
    }

    struct curl_slist* headers = nullptr;
    std::string requestId = GenerateRequestId();
    std::vector<std::string> headerStrings;
    auto addHeader = [&](const std::string& h) {
        headerStrings.push_back(h);
        headers = curl_slist_append(headers, headerStrings.back().c_str());
    };
    addHeader("X-Api-Resource-Id: " + resourceId_);
    addHeader("X-Api-Request-Id: " + requestId);
    addHeader("X-Api-Connect-Id: " + requestId);
    addHeader("X-Api-Sequence: -1");
    if (authMode_ == "api_key") {
        addHeader("X-Api-Key: " + apiKey_);
    } else {
        addHeader("X-Api-App-Key: " + appKey_);
        addHeader("X-Api-Access-Key: " + accessKey_);
    }

    std::string responseHeaders;
    curl_easy_setopt(curl, CURLOPT_URL, endpoint_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "fcitx5-voice-input/0.1.0");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    // 建连也响应 finished：End() 后重试连接没有意义，应立即交出终态
    connectAbort_ = WsAbort::CancelOrFinished(state_->cancelled, state_->finished);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, WsAbortProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &connectAbort_);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
                     +[](char* buf, size_t s, size_t n, void* u) -> size_t {
                         static_cast<std::string*>(u)->append(buf, s * n); return s * n;
                     });
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &responseHeaders);

    CURLcode connectResult = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (state_->cancelled) {
        if (connectResult != CURLE_OK) {
            FCITX_DEBUG() << "[voice-input:volcengine] Connect failed (cancelled): "
                         << curl_easy_strerror(connectResult);
        } else {
            FCITX_DEBUG() << "[voice-input:volcengine] Cancelled during connect";
        }
        curl_easy_cleanup(curl);
        return;
    }

    if (connectResult != CURLE_OK) {
        FCITX_ERROR() << "[voice-input:volcengine] WS connect failed: "
                      << curl_easy_strerror(connectResult);
        if (ecb) ecb("Volcengine connect failed: " + std::string(curl_easy_strerror(connectResult)));
        if (cb) cb("", true, sid);
        curl_easy_cleanup(curl);
        return;
    }

    std::string logId = ExtractHeaderValue(responseHeaders, "X-Tt-Logid:");
    FCITX_INFO() << "[voice-input:volcengine] WS connected session=" << sid << " logId=" << logId;

    // Send full client request
    Json::Value requestJson = BuildRequestJson(modelName_, enableItN_, enablePunc_,
                                               enableDdc_, enableNonstream_, endWindowMs_);
    auto compressedRequest = GzipCompress(JsonToString(requestJson));
    if (compressedRequest.empty()) {
        if (ecb) ecb("Failed to gzip request");
        if (cb) cb("", true, sid);
        CloseWebSocket(curl);
        curl_easy_cleanup(curl);
        return;
    }

    auto requestFrame = BuildFrame(kMsgFullClientRequest, kFlagNoSequence,
                                   kSerializationJson, kCompressionGzip, compressedRequest);
    if (!SendWebSocketBinary(curl, requestFrame.data(), requestFrame.size(),
                             WsAbort::CancelOnly(state_->cancelled))) {
        if (ecb) ecb("Failed to send request");
        if (cb) cb("", true, sid);
        curl_easy_cleanup(curl);
        return;
    }

    // Main streaming loop
    std::vector<int16_t> pendingAudio;
    std::string latestText;
    bool gotFinal = false;
    WsFrameReceiver receiver(CURLWS_BINARY);

    auto handleResponses = [&](std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!state_->cancelled && std::chrono::steady_clock::now() < deadline) {
            std::vector<uint8_t> rawFrame;
            RecvStatus r = ReceiveWebSocketFrame(curl, receiver, rawFrame);
            if (r == RecvStatus::Again) { std::this_thread::sleep_for(10ms); continue; }
            if (r == RecvStatus::Skipped) continue;
            if (r == RecvStatus::Closed) { gotFinal = true; return; }
            if (r == RecvStatus::Error) { if (ecb) ecb("WS recv error"); gotFinal = true; return; }

            ServerMessage msg;
            if (!ParseServerMessage(rawFrame, msg)) continue;
            if (msg.type == kMsgError || msg.payload.empty()) continue;

            Json::Value json;
            Json::Reader reader;
            if (!reader.parse(msg.payload, json)) continue;

            int code = json.get("code", kVolcengineBigmodelSuccessCode).asInt();
            if (code != kVolcengineBigmodelSuccessCode && code != kVolcengineLegacySuccessCode) {
                if (ecb) ecb("API error: " + json.get("message", "").asString());
                gotFinal = true; return;
            }

            std::string text = ExtractTextFromResult(json["result"]);
            if (!text.empty() && text != latestText) {
                latestText = text;
                if (cb) cb(text, false, sid);
            }
            if (msg.flags == kFlagFinalSequence || ResponseHasDefiniteUtterance(json)) {
                gotFinal = true; return;
            }
        }
    };

    while (!state_->cancelled && !gotFinal) {
        std::vector<int16_t> chunk;
        bool hasChunk = audioChunks->TryPop(chunk);
        if (hasChunk && chunk.empty() && state_->finished) {
            // End 路径：结束帧与 wait 共用一份预算，保证 worker 能在
            // SessionReaper 的 15s JoinWithTimeout 之前返回（issue #42）。
            const WsDeadline endBudget = endBudget_.Deadline();
            if (!pendingAudio.empty()) {
                auto compressed = GzipCompress(
                    reinterpret_cast<const uint8_t*>(pendingAudio.data()),
                    pendingAudio.size() * sizeof(int16_t));
                if (!compressed.empty()) {
                    auto frame = BuildFrame(kMsgAudioOnlyRequest, kFlagFinalNoSequence,
                                            kSerializationNone, kCompressionGzip, compressed);
                    SendWebSocketBinary(curl, frame.data(), frame.size(),
                                        WsAbort::CancelOnly(state_->cancelled),
                                        endBudget);
                }
            } else {
                auto frame = BuildFrame(kMsgAudioOnlyRequest, kFlagFinalNoSequence,
                                        kSerializationNone, kCompressionNone, {});
                SendWebSocketBinary(curl, frame.data(), frame.size(),
                                        WsAbort::CancelOnly(state_->cancelled),
                                        endBudget);
            }
            while (!state_->cancelled && !gotFinal && !endBudget.Expired())
                handleResponses(50ms);
            break;
        }
        if (hasChunk && !chunk.empty())
            pendingAudio.insert(pendingAudio.end(), chunk.begin(), chunk.end());

        if (pendingAudio.size() >= chunkSamples && !state_->cancelled) {
            size_t sendSize = (pendingAudio.size() / chunkSamples) * chunkSamples;
            auto compressed = GzipCompress(
                reinterpret_cast<const uint8_t*>(pendingAudio.data()),
                sendSize * sizeof(int16_t));
            if (!compressed.empty()) {
                auto frame = BuildFrame(kMsgAudioOnlyRequest, kFlagNoSequence,
                                        kSerializationNone, kCompressionGzip, compressed);
                SendWebSocketBinary(curl, frame.data(), frame.size(),
                                    WsAbort::CancelOrFinished(
                                        state_->cancelled, state_->finished));
            }
            pendingAudio.erase(pendingAudio.begin(), pendingAudio.begin() + sendSize);
            handleResponses(20ms);
        } else if (!hasChunk) {
            handleResponses(20ms);
            std::this_thread::sleep_for(5ms);
        }
    }

    CloseWebSocket(curl);
    curl_easy_cleanup(curl);

    if (state_->cancelled) {
        FCITX_DEBUG() << "[voice-input:volcengine] Cancelled session=" << sid;
        return;
    }
    if (latestText.empty()) {
        FCITX_WARN() << "[voice-input:volcengine] Empty transcript session=" << sid;
        if (cb) cb("", true, sid);
        return;
    }
    // 转写文本属敏感个人信息，降级为 DEBUG（默认 INFO 级别不落盘内容）
    FCITX_DEBUG() << "[voice-input:volcengine] final session=" << sid
                  << " \"" << latestText << "\"";
    if (cb) cb(latestText, true, sid);
}

// ── VolcengineAsrEngine ──────────────────────────────────────

bool VolcengineAsrEngine::Init(const Config& config) {
    config_ = config;
    return true;
}

AsrSessionStart VolcengineAsrEngine::StartSession() {
    uint64_t sid;
    std::optional<uint64_t> cancelledSessionId;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        cancelledSessionId = CancelOldestSessionIfLimitReachedLocked();
        sid = NextSessionId();
    }

    auto session = std::make_shared<VolcengineAsrSession>(config_, errorCb_, sid);
    session->SetResultCallback(resultCb_);

    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions_[sid] = session;
    }
    return {std::move(session), cancelledSessionId};
}

} // namespace fcitx
