#include "volcengine_asr.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <random>
#include <thread>

#include <curl/curl.h>
#include <fcitx-utils/log.h>
#include <json/json.h>
#include <zlib.h>

using namespace std::chrono_literals;

namespace fcitx {
namespace {

constexpr uint8_t kMsgFullClientRequest = 0x1;
constexpr uint8_t kMsgAudioOnlyRequest = 0x2;
constexpr uint8_t kMsgFullServerResponse = 0x9;
constexpr uint8_t kMsgError = 0xF;
constexpr uint8_t kFlagNoSequence = 0x0;
constexpr uint8_t kFlagPositiveSequence = 0x1;
constexpr uint8_t kFlagFinalNoSequence = 0x2;
constexpr uint8_t kFlagFinalSequence = 0x3;
constexpr uint8_t kSerializationNone = 0x0;
constexpr uint8_t kSerializationJson = 0x1;
constexpr uint8_t kCompressionNone = 0x0;
constexpr uint8_t kCompressionGzip = 0x1;
constexpr int kVolcengineSampleRate = 16000;
constexpr int kVolcengineBigmodelSuccessCode = 20000000;
constexpr int kVolcengineLegacySuccessCode = 1000;

enum class RecvStatus { Ok, Again, Closed, Error };

struct ServerMessage {
    uint8_t type = 0;
    uint8_t flags = 0;
    std::string payload;
};

void AppendUint32Be(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

uint32_t ReadUint32Be(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

std::vector<uint8_t> GzipCompress(const uint8_t* data, size_t size) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return {};
    std::vector<uint8_t> out(deflateBound(&zs, size));
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    zs.avail_in = static_cast<uInt>(size);
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    if (deflate(&zs, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&zs);
        return {};
    }
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

std::vector<uint8_t> GzipCompress(const std::string& data) {
    return GzipCompress(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::string GzipDecompress(const uint8_t* data, size_t size) {
    z_stream zs{};
    if (inflateInit2(&zs, MAX_WBITS + 16) != Z_OK) return {};
    std::string out;
    std::array<char, 8192> buffer{};
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    zs.avail_in = static_cast<uInt>(size);
    int result = Z_OK;
    while (result == Z_OK) {
        zs.next_out = reinterpret_cast<Bytef*>(buffer.data());
        zs.avail_out = static_cast<uInt>(buffer.size());
        result = inflate(&zs, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END) {
            inflateEnd(&zs);
            return {};
        }
        out.append(buffer.data(), buffer.size() - zs.avail_out);
    }
    inflateEnd(&zs);
    return out;
}

std::vector<uint8_t> BuildFrame(uint8_t messageType, uint8_t flags,
                                uint8_t serialization, uint8_t compression,
                                const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    frame.reserve(8 + payload.size());
    frame.push_back(0x11);
    frame.push_back(static_cast<uint8_t>((messageType << 4) | flags));
    frame.push_back(static_cast<uint8_t>((serialization << 4) | compression));
    frame.push_back(0x00);
    AppendUint32Be(frame, static_cast<uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::string GenerateRequestId() {
    std::random_device rd;
    std::uniform_int_distribution<int> hexDist(0, 15);
    std::uniform_int_distribution<int> variantDist(8, 11);
    const char* hex = "0123456789abcdef";
    std::string id(36, '0');
    for (size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            id[i] = '-';
        } else {
            id[i] = hex[hexDist(rd)];
        }
    }
    id[14] = '4';
    id[19] = hex[variantDist(rd)];
    return id;
}

std::string MaskSecret(const std::string& secret) {
    if (secret.empty()) return "(none)";
    if (secret.size() <= 12) return "***";
    return secret.substr(0, 6) + "..." + secret.substr(secret.size() - 4);
}

std::string ExtractHeaderValue(const std::string& headers, const std::string& name) {
    size_t pos = headers.find(name);
    if (pos == std::string::npos) return "";
    pos += name.size();
    while (pos < headers.size() && headers[pos] == ' ') ++pos;
    size_t end = headers.find("\r\n", pos);
    if (end == std::string::npos) end = headers.find('\n', pos);
    return headers.substr(pos, end == std::string::npos ? end : end - pos);
}

bool SendWebSocketBinary(CURL* curl, const uint8_t* data, size_t length,
                         const std::atomic<bool>& cancelFlag) {
    const uint8_t* ptr = data;
    size_t remaining = length;
    while (remaining > 0) {
        if (cancelFlag.load()) return false;
        size_t sent = 0;
        CURLcode result = curl_ws_send(curl, ptr, remaining, &sent, 0, CURLWS_BINARY);
        if (result == CURLE_AGAIN) { std::this_thread::sleep_for(10ms); continue; }
        if (result != CURLE_OK) {
            FCITX_ERROR() << "[voice-input:volcengine] curl_ws_send failed: "
                          << curl_easy_strerror(result);
            return false;
        }
        ptr += sent;
        remaining -= sent;
    }
    return true;
}

RecvStatus ReceiveWebSocketFrame(CURL* curl, std::vector<uint8_t>& frame) {
    frame.clear();
    std::array<uint8_t, 8192> buffer{};
    while (true) {
        size_t received = 0;
        const struct curl_ws_frame* meta = nullptr;
        CURLcode result = curl_ws_recv(curl, buffer.data(), buffer.size(), &received, &meta);
        if (result == CURLE_AGAIN) return RecvStatus::Again;
        if (result != CURLE_OK) {
            FCITX_ERROR() << "[voice-input:volcengine] curl_ws_recv failed: "
                          << curl_easy_strerror(result);
            return RecvStatus::Error;
        }
        if (meta && (meta->flags & CURLWS_CLOSE)) return RecvStatus::Closed;
        if (!meta || !(meta->flags & CURLWS_BINARY)) continue;
        frame.insert(frame.end(), buffer.begin(), buffer.begin() + received);
        if (meta->bytesleft == 0) return RecvStatus::Ok;
    }
}

bool ParseServerMessage(const std::vector<uint8_t>& frame, ServerMessage& msg) {
    if (frame.size() < 8) return false;
    uint8_t version = frame[0] >> 4;
    size_t headerSize = (frame[0] & 0x0f) * 4;
    if (version != 1 || frame.size() < headerSize + 4) return false;
    msg.type = frame[1] >> 4;
    msg.flags = frame[1] & 0x0f;
    uint8_t compression = frame[2] & 0x0f;
    size_t offset = headerSize;
    if (msg.type == kMsgFullServerResponse &&
        (msg.flags == kFlagPositiveSequence || msg.flags == kFlagFinalSequence)) {
        if (frame.size() < offset + 4) return false; offset += 4;
    } else if (msg.type == kMsgError) {
        if (frame.size() < offset + 4) return false; offset += 4;
    }
    if (frame.size() < offset + 4) return false;
    uint32_t payloadSize = ReadUint32Be(frame.data() + offset);
    offset += 4;
    if (frame.size() < offset + payloadSize) return false;
    if (compression == kCompressionGzip) {
        msg.payload = GzipDecompress(frame.data() + offset, payloadSize);
    } else {
        msg.payload.assign(reinterpret_cast<const char*>(frame.data() + offset), payloadSize);
    }
    return !msg.payload.empty() || payloadSize == 0;
}

std::string ExtractTextFromResult(const Json::Value& result) {
    if (result.isObject()) {
        std::string text = result.get("text", "").asString();
        if (!text.empty()) return text;
        const Json::Value& utterances = result["utterances"];
        if (utterances.isArray()) {
            std::string joined;
            for (Json::ArrayIndex i = 0; i < utterances.size(); ++i)
                joined += utterances[i].get("text", "").asString();
            return joined;
        }
    }
    if (result.isArray()) {
        std::string joined;
        for (Json::ArrayIndex i = 0; i < result.size(); ++i)
            joined += ExtractTextFromResult(result[i]);
        return joined;
    }
    return "";
}

bool ResponseHasDefiniteUtterance(const Json::Value& json) {
    const Json::Value& result = json["result"];
    if (!result.isObject()) return false;
    const Json::Value& utterances = result["utterances"];
    if (!utterances.isArray() || utterances.empty()) return false;
    for (Json::ArrayIndex i = 0; i < utterances.size(); ++i)
        if (!utterances[i].get("definite", false).asBool()) return false;
    return true;
}

Json::Value BuildRequestJson(const std::string& modelName, bool enableItN,
                             bool enablePunc, bool enableDdc,
                             bool enableNonstream, int endWindowMs) {
    Json::Value root;
    root["user"]["uid"] = "fcitx5-voice-input";
    root["audio"]["format"] = "pcm";
    root["audio"]["codec"] = "raw";
    root["audio"]["rate"] = 16000;
    root["audio"]["bits"] = 16;
    root["audio"]["channel"] = 1;
    root["request"]["model_name"] = modelName.empty() ? "bigmodel" : modelName.c_str();
    root["request"]["enable_itn"] = enableItN;
    root["request"]["enable_punc"] = enablePunc;
    root["request"]["enable_ddc"] = enableDdc;
    root["request"]["show_utterances"] = true;
    root["request"]["result_type"] = "full";
    root["request"]["enable_nonstream"] = enableNonstream;
    root["request"]["end_window_size"] = endWindowMs;
    return root;
}

std::string JsonToString(const Json::Value& json) {
    return Json::FastWriter().write(json);
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
    audioChunks_ = std::make_shared<ThreadSafeQueue<std::vector<int16_t>>>();

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

    FCITX_INFO() << "[voice-input:volcengine] Init session=" << sessionId
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
        workerThread_ = std::make_unique<std::thread>([self]() { self->WorkerLoop(); });
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
    state_->finished = true;
    if (audioChunks_) audioChunks_->Push(std::vector<int16_t>());
}

void VolcengineAsrSession::Cancel() {
    state_->cancelled = true;
    if (audioChunks_) audioChunks_->Push(std::vector<int16_t>());
}

void VolcengineAsrSession::JoinWithTimeout(std::chrono::milliseconds timeout) {
    if (workerThread_ && workerThread_->joinable()) {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < timeout) {
            std::this_thread::sleep_for(10ms);
            if (!workerThread_->joinable()) { workerThread_->join(); return; }
        }
        FCITX_WARN() << "[voice-input:volcengine] Join timeout session=" << state_->sessionId;
        workerThread_->detach();
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
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
                     +[](char* buf, size_t s, size_t n, void* u) -> size_t {
                         static_cast<std::string*>(u)->append(buf, s * n); return s * n;
                     });
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &responseHeaders);

    CURLcode connectResult = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (state_->cancelled) {
        if (connectResult != CURLE_OK) {
            FCITX_INFO() << "[voice-input:volcengine] Connect failed (cancelled): "
                         << curl_easy_strerror(connectResult);
        } else {
            FCITX_INFO() << "[voice-input:volcengine] Cancelled during connect";
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
    if (!SendWebSocketBinary(curl, requestFrame.data(), requestFrame.size(), state_->cancelled)) {
        if (ecb) ecb("Failed to send request");
        if (cb) cb("", true, sid);
        curl_easy_cleanup(curl);
        return;
    }

    // Main streaming loop
    std::vector<int16_t> pendingAudio;
    std::string latestText;
    bool gotFinal = false;

    auto handleResponses = [&](std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!state_->cancelled && std::chrono::steady_clock::now() < deadline) {
            std::vector<uint8_t> rawFrame;
            RecvStatus r = ReceiveWebSocketFrame(curl, rawFrame);
            if (r == RecvStatus::Again) { std::this_thread::sleep_for(10ms); continue; }
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
            if (!pendingAudio.empty()) {
                auto compressed = GzipCompress(
                    reinterpret_cast<const uint8_t*>(pendingAudio.data()),
                    pendingAudio.size() * sizeof(int16_t));
                if (!compressed.empty()) {
                    auto frame = BuildFrame(kMsgAudioOnlyRequest, kFlagFinalNoSequence,
                                            kSerializationNone, kCompressionGzip, compressed);
                    SendWebSocketBinary(curl, frame.data(), frame.size(), state_->cancelled);
                }
            } else {
                auto frame = BuildFrame(kMsgAudioOnlyRequest, kFlagFinalNoSequence,
                                        kSerializationNone, kCompressionNone, {});
                SendWebSocketBinary(curl, frame.data(), frame.size(), state_->cancelled);
            }
            auto deadline = std::chrono::steady_clock::now() + 30s;
            while (!state_->cancelled && !gotFinal && std::chrono::steady_clock::now() < deadline)
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
                SendWebSocketBinary(curl, frame.data(), frame.size(), state_->cancelled);
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
        FCITX_INFO() << "[voice-input:volcengine] Cancelled session=" << sid;
        return;
    }
    if (latestText.empty()) {
        FCITX_WARN() << "[voice-input:volcengine] Empty transcript session=" << sid;
        if (cb) cb("", true, sid);
        return;
    }
    FCITX_INFO() << "[voice-input:volcengine] final session=" << sid << " \"" << latestText << "\"";
    if (cb) cb(latestText, true, sid);
}

// ── VolcengineAsrEngine ──────────────────────────────────────

bool VolcengineAsrEngine::Init(const Config& config) {
    config_ = config;
    return true;
}

std::shared_ptr<AsrSession> VolcengineAsrEngine::StartSession() {
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        // Clean expired weak_ptrs
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            if (it->second.expired()) it = sessions_.erase(it);
            else ++it;
        }
        // Enforce maxActiveSessions: cancel oldest
        while (sessions_.size() >= maxActiveSessions_) {
            auto oldest = sessions_.begin();
            for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
                auto s = it->second.lock();
                auto o = oldest->second.lock();
                if (s && (!o || s->GetState()->sessionId < o->GetState()->sessionId))
                    oldest = it;
            }
            auto s = oldest->second.lock();
            if (s) { FCITX_WARN() << "[voice-input:volcengine] Too many sessions, cancel oldest"; s->Cancel(); }
            sessions_.erase(oldest);
        }
    }

    uint64_t sid;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sid = nextSessionId_++;
    }

    auto session = std::make_shared<VolcengineAsrSession>(config_, errorCb_, sid);
    session->SetResultCallback(resultCb_);
    if (!session->GetState()->finished)
        session->StartWorker();

    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions_[sid] = session;
    }
    return session;
}

} // namespace fcitx
