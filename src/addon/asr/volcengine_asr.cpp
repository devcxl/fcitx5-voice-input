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

enum class RecvStatus {
    Ok,
    Again,
    Closed,
    Error,
};

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
                     MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }

    std::vector<uint8_t> out(deflateBound(&zs, size));
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    zs.avail_in = static_cast<uInt>(size);
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());

    int result = deflate(&zs, Z_FINISH);
    if (result != Z_STREAM_END) {
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
    if (inflateInit2(&zs, MAX_WBITS + 16) != Z_OK) {
        return {};
    }

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
    while (pos < headers.size() && headers[pos] == ' ') {
        ++pos;
    }

    size_t end = headers.find("\r\n", pos);
    if (end == std::string::npos) {
        end = headers.find('\n', pos);
    }
    return headers.substr(pos, end == std::string::npos ? end : end - pos);
}

bool SendWebSocketBinary(CURL* curl, const std::vector<uint8_t>& data,
                         const std::atomic<bool>& cancelFlag) {
    const uint8_t* ptr = data.data();
    size_t remaining = data.size();
    while (remaining > 0) {
        if (cancelFlag.load()) return false;
        size_t sent = 0;
        CURLcode result = curl_ws_send(curl, ptr, remaining, &sent, 0, CURLWS_BINARY);
        if (result == CURLE_AGAIN) {
            std::this_thread::sleep_for(10ms);
            continue;
        }
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
        CURLcode result = curl_ws_recv(curl, buffer.data(), buffer.size(),
                                       &received, &meta);
        if (result == CURLE_AGAIN) {
            return RecvStatus::Again;
        }
        if (result != CURLE_OK) {
            FCITX_ERROR() << "[voice-input:volcengine] curl_ws_recv failed: "
                          << curl_easy_strerror(result);
            return RecvStatus::Error;
        }
        if (meta && (meta->flags & CURLWS_CLOSE)) {
            return RecvStatus::Closed;
        }
        if (!meta || !(meta->flags & CURLWS_BINARY)) {
            continue;
        }

        frame.insert(frame.end(), buffer.begin(), buffer.begin() + received);
        if (meta->bytesleft == 0) {
            return RecvStatus::Ok;
        }
    }
}

void CloseWebSocket(CURL* curl) {
    size_t sent = 0;
    CURLcode result = curl_ws_send(curl, "", 0, &sent, 0, CURLWS_CLOSE);
    if (result != CURLE_OK) {
        FCITX_WARN() << "[voice-input:volcengine] WebSocket close failed: "
                     << curl_easy_strerror(result);
    }
}

bool ParseServerMessage(const std::vector<uint8_t>& frame, ServerMessage& message) {
    if (frame.size() < 8) return false;

    uint8_t version = frame[0] >> 4;
    size_t headerSize = (frame[0] & 0x0f) * 4;
    if (version != 1 || frame.size() < headerSize + 4) return false;

    message.type = frame[1] >> 4;
    message.flags = frame[1] & 0x0f;
    uint8_t compression = frame[2] & 0x0f;

    size_t offset = headerSize;
    if (message.type == kMsgFullServerResponse &&
        (message.flags == kFlagPositiveSequence || message.flags == kFlagFinalSequence)) {
        if (frame.size() < offset + 4) return false;
        offset += 4;
    } else if (message.type == kMsgError) {
        if (frame.size() < offset + 4) return false;
        uint32_t errorCode = ReadUint32Be(frame.data() + offset);
        FCITX_ERROR() << "[voice-input:volcengine] server error code=" << errorCode;
        offset += 4;
    }

    if (frame.size() < offset + 4) return false;
    uint32_t payloadSize = ReadUint32Be(frame.data() + offset);
    offset += 4;
    if (frame.size() < offset + payloadSize) return false;

    if (compression == kCompressionGzip) {
        message.payload = GzipDecompress(frame.data() + offset, payloadSize);
    } else if (compression == kCompressionNone) {
        message.payload.assign(reinterpret_cast<const char*>(frame.data() + offset), payloadSize);
    } else {
        return false;
    }
    return !message.payload.empty() || payloadSize == 0;
}

std::string ExtractTextFromResult(const Json::Value& result) {
    if (result.isObject()) {
        std::string text = result.get("text", Json::Value("")).asString();
        if (!text.empty()) return text;

        const Json::Value& utterances = result["utterances"];
        if (utterances.isArray()) {
            std::string joined;
            for (Json::ArrayIndex i = 0; i < utterances.size(); ++i) {
                const Json::Value& utterance = utterances[i];
                joined += utterance.get("text", Json::Value("")).asString();
            }
            return joined;
        }
    }

    if (result.isArray()) {
        std::string joined;
        for (Json::ArrayIndex i = 0; i < result.size(); ++i) {
            joined += ExtractTextFromResult(result[i]);
        }
        return joined;
    }

    return "";
}

bool ResponseHasDefiniteUtterance(const Json::Value& json) {
    const Json::Value& result = json["result"];
    if (!result.isObject()) return false;

    const Json::Value& utterances = result["utterances"];
    if (!utterances.isArray() || utterances.empty()) return false;

    for (Json::ArrayIndex i = 0; i < utterances.size(); ++i) {
        const Json::Value& utterance = utterances[i];
        if (!utterance.get("definite", Json::Value(false)).asBool()) {
            return false;
        }
    }
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
    root["request"]["model_name"] = modelName.empty()
                                      ? Json::Value("bigmodel")
                                      : Json::Value(modelName.c_str());
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
    Json::FastWriter writer;
    return writer.write(json);
}

} // namespace

VolcengineStreamingAsrEngine::VolcengineStreamingAsrEngine() = default;

VolcengineStreamingAsrEngine::~VolcengineStreamingAsrEngine() {
    cancelled_ = true;
    if (workerThread_ && workerThread_->joinable()) {
        workerThread_->join();
    }
}

bool VolcengineStreamingAsrEngine::Init(const Config& config) {
    endpoint_ = config.apiEndpoint.empty()
                    ? "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
                    : config.apiEndpoint;
    authMode_ = config.authMode.empty() ? "api_key" : config.authMode;
    apiKey_ = config.apiKey;
    appKey_ = config.appKey;
    accessKey_ = config.accessKey;
    resourceId_ = config.resourceId.empty()
                      ? "volc.seedasr.sauc.duration"
                      : config.resourceId;
    modelName_ = config.modelName.empty() ? "bigmodel" : config.modelName;
    chunkMs_ = std::clamp(config.chunkMs, 100, 200);
    enableItN_ = config.enableItN;
    enablePunc_ = config.enablePunc;
    enableDdc_ = config.enableDdc;
    enableNonstream_ = config.enableNonstream;
    endWindowMs_ = std::clamp(config.endWindowMs, 200, 3000);

    if (authMode_ == "api_key") {
        if (apiKey_.empty()) {
            FCITX_ERROR() << "[voice-input:volcengine] API key not configured";
            return false;
        }
    } else if (authMode_ == "app_access_key") {
        if (appKey_.empty() || accessKey_.empty()) {
            FCITX_ERROR() << "[voice-input:volcengine] App key/access key not configured";
            return false;
        }
    } else {
        FCITX_ERROR() << "[voice-input:volcengine] Unsupported auth mode: " << authMode_;
        return false;
    }

    FCITX_INFO() << "[voice-input:volcengine] Init: endpoint=" << endpoint_
                 << " resourceId=" << resourceId_
                 << " authMode=" << authMode_
                 << " apiKey=" << MaskSecret(apiKey_)
                 << " appKey=" << MaskSecret(appKey_)
                 << " chunkMs=" << chunkMs_;
    return true;
}

void VolcengineStreamingAsrEngine::Start() {
    cancelled_ = false;
    finished_ = false;

    // Drain any stale chunks
    std::vector<int16_t> stale;
    while (audioChunks_.TryPop(stale)) {}

    workerThread_ = std::make_unique<std::thread>(
        &VolcengineStreamingAsrEngine::WorkerLoop, this);
    FCITX_INFO() << "[voice-input:volcengine] Start worker thread";
}

void VolcengineStreamingAsrEngine::FeedAudio(const float* pcm, size_t frames) {
    std::vector<int16_t> chunk(frames);
    for (size_t i = 0; i < frames; ++i) {
        float sample = std::clamp(pcm[i], -1.0f, 1.0f);
        chunk[i] = static_cast<int16_t>(sample * 32767.0f);
    }
    audioChunks_.Push(std::move(chunk));
}

void VolcengineStreamingAsrEngine::Stop() {
    finished_ = true;
    audioChunks_.Push(std::vector<int16_t>());

    if (workerThread_ && workerThread_->joinable()) {
        workerThread_->join();
        workerThread_.reset();
    }
}

void VolcengineStreamingAsrEngine::WorkerLoop() {
    const size_t chunkSamples = static_cast<size_t>(
        kVolcengineSampleRate * chunkMs_ / 1000);

    CURL* curl = curl_easy_init();
    if (!curl) {
        if (errorCb_) errorCb_("Failed to initialize libcurl");
        if (resultCb_) resultCb_("", true);
        return;
    }

    struct curl_slist* headers = nullptr;
    const std::string requestId = GenerateRequestId();
    std::vector<std::string> headerStrings;
    headerStrings.reserve(16);
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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "fcitx5-voice-input/0.1.0");
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
                     +[](char* buffer, size_t size, size_t nitems, void* userdata) -> size_t {
                         auto* hdrs = static_cast<std::string*>(userdata);
                         hdrs->append(buffer, size * nitems);
                         return size * nitems;
                     });
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &responseHeaders);

    CURLcode connectResult = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (connectResult != CURLE_OK || cancelled_) {
        FCITX_ERROR() << "[voice-input:volcengine] WS connect failed: "
                      << curl_easy_strerror(connectResult)
                      << " endpoint=" << endpoint_;
        if (errorCb_) errorCb_("Volcengine connect failed: " +
                               std::string(curl_easy_strerror(connectResult)));
        if (resultCb_) resultCb_("", true);
        curl_easy_cleanup(curl);
        return;
    }

    std::string logId = ExtractHeaderValue(responseHeaders, "X-Tt-Logid:");
    FCITX_INFO() << "[voice-input:volcengine] WS connected requestId="
                 << requestId << " logId=" << logId;

    // Send full client request
    Json::Value requestJson = BuildRequestJson(modelName_, enableItN_, enablePunc_,
                                               enableDdc_, enableNonstream_, endWindowMs_);
    std::vector<uint8_t> compressedRequest = GzipCompress(JsonToString(requestJson));
    if (compressedRequest.empty()) {
        FCITX_ERROR() << "[voice-input:volcengine] Failed to gzip request";
        if (errorCb_) errorCb_("Failed to gzip request");
        if (resultCb_) resultCb_("", true);
        CloseWebSocket(curl);
        curl_easy_cleanup(curl);
        return;
    }

    std::vector<uint8_t> requestFrame = BuildFrame(
        kMsgFullClientRequest, kFlagNoSequence, kSerializationJson,
        kCompressionGzip, compressedRequest);
    if (!SendWebSocketBinary(curl, requestFrame, cancelled_)) {
        FCITX_ERROR() << "[voice-input:volcengine] Failed to send request frame";
        if (errorCb_) errorCb_("Failed to send Volcengine request");
        if (resultCb_) resultCb_("", true);
        curl_easy_cleanup(curl);
        return;
    }

    // Main streaming loop
    std::vector<int16_t> pendingAudio;
    std::string latestText;
    bool gotFinal = false;

    auto handleResponses = [&](std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!cancelled_ && std::chrono::steady_clock::now() < deadline) {
            std::vector<uint8_t> rawFrame;
            RecvStatus r = ReceiveWebSocketFrame(curl, rawFrame);
            if (r == RecvStatus::Again) {
                std::this_thread::sleep_for(10ms);
                continue;
            }
            if (r == RecvStatus::Closed) { gotFinal = true; return; }
            if (r == RecvStatus::Error) {
                if (errorCb_) errorCb_("Volcengine WS receive failed");
                gotFinal = true; return;
            }

            ServerMessage msg;
            if (!ParseServerMessage(rawFrame, msg)) continue;

            if (msg.type == kMsgError) {
                if (errorCb_) errorCb_("Volcengine error: " + msg.payload);
                gotFinal = true; return;
            }
            if (msg.type != kMsgFullServerResponse || msg.payload.empty()) continue;

            Json::Value json;
            Json::Reader reader;
            if (!reader.parse(msg.payload, json)) continue;

            int code = json.get("code", Json::Value(kVolcengineBigmodelSuccessCode)).asInt();
            if (code != kVolcengineBigmodelSuccessCode && code != kVolcengineLegacySuccessCode) {
                if (errorCb_) errorCb_("Volcengine API error: " +
                                       json.get("message", Json::Value("")).asString());
                gotFinal = true; return;
            }

            std::string text = ExtractTextFromResult(json["result"]);
            if (!text.empty() && text != latestText) {
                latestText = text;
                if (resultCb_) resultCb_(text, false);
            }

            if (msg.flags == kFlagFinalSequence || ResponseHasDefiniteUtterance(json)) {
                gotFinal = true;
                return;
            }
        }
    };

    while (!cancelled_ && !gotFinal) {
        std::vector<int16_t> chunk;
        bool hasChunk = audioChunks_.TryPop(chunk);
        if (hasChunk && chunk.empty() && finished_) {
            // Final marker from Stop() — flush pending and send final
            if (!pendingAudio.empty()) {
                auto* bytes = reinterpret_cast<const uint8_t*>(pendingAudio.data());
                std::vector<uint8_t> compressed = GzipCompress(bytes, pendingAudio.size() * sizeof(int16_t));
                if (!compressed.empty()) {
                    std::vector<uint8_t> frame = BuildFrame(
                        kMsgAudioOnlyRequest, kFlagFinalNoSequence,
                        kSerializationNone, kCompressionGzip, compressed);
                    SendWebSocketBinary(curl, frame, cancelled_);
                }
            } else {
                // Send empty final audio to signal end of stream
                std::vector<uint8_t> frame = BuildFrame(
                    kMsgAudioOnlyRequest, kFlagFinalNoSequence,
                    kSerializationNone, kCompressionNone, {});
                SendWebSocketBinary(curl, frame, cancelled_);
            }

            auto finalDeadline = std::chrono::steady_clock::now() + 30s;
            while (!cancelled_ && !gotFinal && std::chrono::steady_clock::now() < finalDeadline) {
                handleResponses(50ms);
            }
            break;
        }

        if (hasChunk && !chunk.empty()) {
            pendingAudio.insert(pendingAudio.end(), chunk.begin(), chunk.end());
        }

        // Send when we have enough or if we've been accumulating without sending
        bool shouldSend = pendingAudio.size() >= chunkSamples;
        if (shouldSend && !cancelled_) {
            size_t sendSize = std::min(pendingAudio.size(), chunkSamples);
            auto* bytes = reinterpret_cast<const uint8_t*>(pendingAudio.data());
            std::vector<uint8_t> compressed = GzipCompress(bytes, sendSize * sizeof(int16_t));
            if (!compressed.empty()) {
                std::vector<uint8_t> frame = BuildFrame(
                    kMsgAudioOnlyRequest, kFlagNoSequence,
                    kSerializationNone, kCompressionGzip, compressed);
                SendWebSocketBinary(curl, frame, cancelled_);
            }
            if (sendSize < pendingAudio.size()) {
                pendingAudio.erase(pendingAudio.begin(),
                                   pendingAudio.begin() + sendSize);
            } else {
                pendingAudio.clear();
            }
            handleResponses(20ms);
        } else if (!hasChunk) {
            handleResponses(20ms);
            std::this_thread::sleep_for(5ms);
        }
    }

    CloseWebSocket(curl);
    curl_easy_cleanup(curl);

    if (cancelled_) {
        FCITX_INFO() << "[voice-input:volcengine] Cancelled";
        return;
    }

    if (latestText.empty()) {
        FCITX_WARN() << "[voice-input:volcengine] Empty transcript";
        if (resultCb_) resultCb_("", true);
        return;
    }

    FCITX_INFO() << "[voice-input:volcengine] final: \"" << latestText << "\"";
    if (resultCb_) resultCb_(latestText, true);
}

} // namespace fcitx
