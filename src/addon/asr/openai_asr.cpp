#include "openai_asr.h"

#include <chrono>
#include <cstring>
#include <sstream>

#include <curl/curl.h>
#include <fcitx-utils/log.h>
#include <json/json.h>

namespace fcitx {
namespace {

// ── WAV writing helpers ─────────────────────────────────────────────
#pragma pack(push, 1)
struct WavHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t fileSize = 0;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 1;       // PCM
    uint16_t numChannels = 1;       // mono
    uint32_t sampleRate = 16000;
    uint32_t byteRate = 32000;      // sampleRate * 1 * 2
    uint16_t blockAlign = 2;        // 1 * 2
    uint16_t bitsPerSample = 16;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t dataSize = 0;
};

std::vector<uint8_t> FloatPcmToWav(const float* pcm, size_t frames) {
    WavHeader header;
    size_t dataBytes = frames * sizeof(int16_t);
    header.fileSize = static_cast<uint32_t>(sizeof(WavHeader) - 8 + dataBytes);
    header.dataSize = static_cast<uint32_t>(dataBytes);

    std::vector<uint8_t> wav(sizeof(WavHeader) + dataBytes);
    std::memcpy(wav.data(), &header, sizeof(WavHeader));

    auto* samplePtr = reinterpret_cast<int16_t*>(wav.data() + sizeof(WavHeader));
    for (size_t i = 0; i < frames; ++i) {
        float s = pcm[i];
        // Clamp to [-1, 1]
        if (s < -1.0f) s = -1.0f;
        if (s > 1.0f) s = 1.0f;
        samplePtr[i] = static_cast<int16_t>(s * 32767.0f);
    }

    return wav;
}

// ── Curl write callback ─────────────────────────────────────────────
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), total);
    return total;
}

} // anonymous namespace

// ── OpenaiCompatAsrEngine ──────────────────────────────────────────

OpenaiCompatAsrEngine::OpenaiCompatAsrEngine() = default;

OpenaiCompatAsrEngine::~OpenaiCompatAsrEngine() {
    cancelled_ = true;
    if (workerThread_ && workerThread_->joinable()) {
        workerThread_->join();
    }
}

bool OpenaiCompatAsrEngine::Init(const Config& config) {
    apiEndpoint_ = config.apiEndpoint;
    apiKey_ = config.apiKey;
    modelName_ = config.modelName;
    language_ = config.language;

    if (apiEndpoint_.empty()) {
        apiEndpoint_ = "https://api.openai.com/v1";
    }
    if (modelName_.empty()) {
        modelName_ = "whisper-1";
    }
    if (language_.empty()) {
        language_ = "zh";
    }

    // Log config (mask API key)
    std::string maskedKey = apiKey_.empty() ? "(none)" :
        apiKey_.substr(0, 8) + "..." + apiKey_.substr(apiKey_.size() - 4);
    FCITX_INFO() << "[voice-input:openai] Init: endpoint=" << apiEndpoint_
                 << " model=" << modelName_ << " language=" << language_
                 << " apiKey=" << maskedKey;

    // Validate: we need at least an API key
    if (apiKey_.empty()) {
        FCITX_ERROR() << "[voice-input:openai] API key not configured";
        return false;
    }

    return true;
}

void OpenaiCompatAsrEngine::Start() {
    pcmBuffer_.clear();
    FCITX_DEBUG() << "[voice-input:openai] Start (buffer cleared)";
}

void OpenaiCompatAsrEngine::FeedAudio(const float* pcm, size_t frames) {
    pcmBuffer_.insert(pcmBuffer_.end(), pcm, pcm + frames);
    float durSec = pcmBuffer_.size() / 16000.0f;
    if (static_cast<int>(pcmBuffer_.size()) % 16000 < static_cast<int>(frames)) {
        FCITX_DEBUG() << "[voice-input:openai] FeedAudio: buffer=" << pcmBuffer_.size()
                      << " frames (" << durSec << "s)";
    }
}

void OpenaiCompatAsrEngine::Stop() {
    cancelled_ = true;
    if (workerThread_ && workerThread_->joinable()) {
        FCITX_DEBUG() << "[voice-input:openai] Joining previous worker thread";
        workerThread_->join();
    }

    if (pcmBuffer_.empty()) {
        FCITX_WARN() << "[voice-input:openai] Stop with empty buffer — no audio to transcribe";
        if (resultCb_) {
            resultCb_("", true);
        }
        return;
    }

    float durSec = pcmBuffer_.size() / 16000.0f;
    FCITX_INFO() << "[voice-input:openai] Stop: " << pcmBuffer_.size()
                 << " frames (" << durSec << "s), starting transcription";

    cancelled_ = false;
    workerThread_ = std::make_unique<std::thread>(
        &OpenaiCompatAsrEngine::TranscribeWorker, this);
}

void OpenaiCompatAsrEngine::TranscribeWorker() {
    auto finishEmpty = [this]() {
        if (resultCb_) {
            resultCb_("", true);
        }
    };

    // Take ownership of the buffer
    std::vector<float> audio;
    std::swap(audio, pcmBuffer_);

    size_t audioFrames = audio.size();
    float audioDurSec = audioFrames / 16000.0f;
    FCITX_INFO() << "[voice-input:openai] TranscribeWorker: processing "
                 << audioFrames << " frames (" << audioDurSec << "s)";

    if (cancelled_) {
        FCITX_INFO() << "[voice-input:openai] Cancelled before WAV encoding";
        return;
    }

    // Encode to WAV
    auto encodeStart = std::chrono::steady_clock::now();
    std::vector<uint8_t> wavData = FloatPcmToWav(audio.data(), audio.size());
    auto encodeEnd = std::chrono::steady_clock::now();
    auto encodeUs = std::chrono::duration_cast<std::chrono::microseconds>(encodeEnd - encodeStart).count();
    FCITX_DEBUG() << "[voice-input:openai] WAV encoding: " << wavData.size()
                  << " bytes in " << encodeUs << "us (" << (encodeUs / 1000) << "ms)";

    if (cancelled_) {
        FCITX_INFO() << "[voice-input:openai] Cancelled before HTTP request";
        return;
    }

    // Make HTTP request
    auto httpStart = std::chrono::steady_clock::now();
    std::string response = DoHttpRequest(wavData);
    auto httpEnd = std::chrono::steady_clock::now();
    auto httpMs = std::chrono::duration_cast<std::chrono::milliseconds>(httpEnd - httpStart).count();

    if (cancelled_) {
        FCITX_INFO() << "[voice-input:openai] Cancelled after HTTP request";
        return;
    }

    FCITX_INFO() << "[voice-input:openai] HTTP response: " << response.size()
                 << " bytes in " << httpMs << "ms";

    if (response.empty()) {
        FCITX_ERROR() << "[voice-input:openai] Empty response from API";
        if (errorCb_) {
            errorCb_("Empty response from API");
        }
        finishEmpty();
        return;
    }

    // Parse JSON response
    Json::Value json;
    Json::Reader reader;
    if (!reader.parse(response, json)) {
        FCITX_ERROR() << "[voice-input:openai] JSON parse error: "
                       << reader.getFormattedErrorMessages()
                       << " response=" << response.substr(0, 200);
        if (errorCb_) {
            errorCb_("JSON parse error");
        }
        finishEmpty();
        return;
    }

    // Check for error
    if (json.isMember("error")) {
        std::string errMsg = json["error"].get("message", Json::Value("unknown error")).asString();
        FCITX_ERROR() << "[voice-input:openai] API error: " << errMsg;
        if (errorCb_) {
            errorCb_("API error: " + errMsg);
        }
        finishEmpty();
        return;
    }

    std::string text = json.get("text", Json::Value("")).asString();
    FCITX_INFO() << "[voice-input:openai] transcript: \""
                 << text << "\" (" << text.size() << " chars)";
    if (resultCb_) {
        resultCb_(text, true);
    }
}

std::string OpenaiCompatAsrEngine::DoHttpRequest(const std::vector<uint8_t>& wavData) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        FCITX_ERROR() << "[voice-input:openai] Failed to initialize libcurl";
        if (errorCb_) errorCb_("Failed to initialize libcurl");
        return "";
    }

    std::string response;
    struct curl_slist* headers = nullptr;

    // Build URL: {endpoint}/audio/transcriptions
    std::string url = apiEndpoint_;
    if (!url.empty() && url.back() != '/') {
        url += '/';
    }
    url += "audio/transcriptions";

    FCITX_INFO() << "[voice-input:openai] POST " << url
                 << " (wav=" << wavData.size() << " bytes)";

    // Set up multipart form
    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part;

    // Audio file
    part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_data(part, reinterpret_cast<const char*>(wavData.data()), wavData.size());
    curl_mime_filename(part, "audio.wav");
    curl_mime_type(part, "audio/wav");

    // Model
    part = curl_mime_addpart(mime);
    curl_mime_name(part, "model");
    curl_mime_data(part, modelName_.c_str(), CURL_ZERO_TERMINATED);

    // Language
    part = curl_mime_addpart(mime);
    curl_mime_name(part, "language");
    curl_mime_data(part, language_.c_str(), CURL_ZERO_TERMINATED);


    // Response format
    part = curl_mime_addpart(mime);
    curl_mime_name(part, "response_format");
    curl_mime_data(part, "json", CURL_ZERO_TERMINATED);

    // Headers
    std::string authHeader = "Authorization: Bearer " + apiKey_;
    headers = curl_slist_append(headers, authHeader.c_str());

    // Curl options
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "fcitx5-voice-input/0.1.0");
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                     +[](void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                         auto* self = static_cast<OpenaiCompatAsrEngine*>(clientp);
                         return self->cancelled_ ? 1 : 0;
                     });
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    // Also grab the Content-Type header to detect errors in non-JSON responses
    std::string contentType;
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
                     +[](void* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
                         auto* ct = static_cast<std::string*>(userdata);
                         std::string line(static_cast<char*>(ptr), size * nmemb);
                         if (line.rfind("Content-Type:", 0) == 0) {
                             *ct = line.substr(line.find(':') + 1);
                             if (!ct->empty() && (*ct)[0] == ' ') ct->erase(0, 1);
                             if (!ct->empty() && (*ct).back() == '\r') ct->pop_back();
                             if (!ct->empty() && (*ct).back() == '\n') ct->pop_back();
                         }
                         return size * nmemb;
                     });
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &contentType);

    // Perform request
    CURLcode res = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    // Cleanup
    curl_mime_free(mime);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res == CURLE_ABORTED_BY_CALLBACK) {
        return "";
    }
    if (res != CURLE_OK) {
        std::string err = curl_easy_strerror(res);
        FCITX_ERROR() << "[voice-input:openai] HTTP request failed: " << err
                      << " (url=" << url << ")";
        if (errorCb_) errorCb_("HTTP request failed: " + err);
        return "";
    }

    FCITX_DEBUG() << "[voice-input:openai] HTTP " << httpCode
                  << " response=" << response.size() << " bytes"
                  << " contentType=" << contentType;

    if (httpCode != 200) {
        std::string errMsg = "HTTP " + std::to_string(httpCode);
        if (!response.empty()) {
            // Try to extract error message from response
            Json::Value ejson;
            Json::Reader ereader;
            if (ereader.parse(response, ejson) && ejson.isMember("error")) {
                errMsg += ": " + ejson["error"].get("message", Json::Value(response)).asString();
            } else {
                errMsg += ": " + response;
            }
        }
        FCITX_ERROR() << "[voice-input:openai] API error: " << errMsg;
        if (errorCb_) errorCb_("API error: " + errMsg);
        return "";
    }

    return response;
}

} // namespace fcitx
