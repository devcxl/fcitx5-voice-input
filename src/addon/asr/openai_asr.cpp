#include "openai_asr.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>

#include <curl/curl.h>
#include <fcitx-utils/log.h>
#include <json/json.h>

using namespace std::chrono_literals;

namespace fcitx {
namespace {

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
#pragma pack(pop)

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
        if (s < -1.0f) s = -1.0f;
        if (s > 1.0f) s = 1.0f;
        samplePtr[i] = static_cast<int16_t>(s * 32767.0f);
    }
    return wav;
}

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* response = static_cast<std::string*>(userp);
    response->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

std::string BuildMultipartBody(const std::vector<uint8_t>& wavData,
                               const std::string& model,
                               const std::string& language,
                               const std::string& boundary) {
    std::ostringstream body;

    body << "--" << boundary << "\r\n"
         << "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
         << model << "\r\n";

    if (!language.empty()) {
        body << "--" << boundary << "\r\n"
             << "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
             << language << "\r\n";
    }

    body << "--" << boundary << "\r\n"
         << "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
         << "Content-Type: audio/wav\r\n\r\n";

    auto bodyStr = body.str();

    std::string result;
    result.reserve(bodyStr.size() + wavData.size() + boundary.size() + 8);
    result += bodyStr;
    result.append(reinterpret_cast<const char*>(wavData.data()), wavData.size());
    result += "\r\n--" + boundary + "--\r\n";

    return result;
}

} // namespace

// ── OpenaiAsrSession ─────────────────────────────────────────

OpenaiAsrSession::OpenaiAsrSession(const AsrEngine::Config& config,
                                   ResultCallback resultCb,
                                   ErrorCallback errorCb,
                                   uint64_t sessionId) {
    state_->sessionId = sessionId;
    resultCb_ = std::move(resultCb);
    errorCb_ = std::move(errorCb);

    apiEndpoint_ = config.apiEndpoint;
    apiKey_ = config.apiKey;
    modelName_ = config.modelName.empty() ? "whisper-1" : config.modelName;
    language_ = config.language;

    FCITX_INFO() << "[voice-input:openai] Init session=" << sessionId
                 << " endpoint=" << apiEndpoint_
                 << " model=" << modelName_;
}

OpenaiAsrSession::~OpenaiAsrSession() {
    Cancel();
    JoinWithTimeout(5s);
}

void OpenaiAsrSession::FeedAudio(const float* pcm, size_t frames) {
    if (state_->cancelled) return;
    std::lock_guard<std::mutex> lock(bufferMutex_);
    pcmBuffer_.insert(pcmBuffer_.end(), pcm, pcm + frames);
}

void OpenaiAsrSession::End() {
    state_->finished = true;
    std::vector<float> buffer;
    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        if (pcmBuffer_.empty()) return;
        buffer.swap(pcmBuffer_);
    }
    workerThread_ = std::make_unique<std::thread>(
        [this, buf = std::move(buffer)]() mutable {
            TranscribeWorker(std::move(buf));
        });
}

void OpenaiAsrSession::Cancel() {
    state_->cancelled = true;
}

void OpenaiAsrSession::JoinWithTimeout(std::chrono::milliseconds timeout) {
    if (workerThread_ && workerThread_->joinable()) {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < timeout) {
            std::this_thread::sleep_for(10ms);
            if (!workerThread_->joinable()) {
                workerThread_->join();
                return;
            }
        }
        FCITX_WARN() << "[voice-input:openai] Join timeout session="
                     << state_->sessionId;
        workerThread_->detach();
    }
    workerThread_.reset();
}

void OpenaiAsrSession::TranscribeWorker(std::vector<float> pcm) {
    auto state = state_; // capture shared_ptr

    // Build WAV
    std::vector<uint8_t> wavData = FloatPcmToWav(pcm.data(), pcm.size());

    std::string endpoint = apiEndpoint_;
    if (!endpoint.empty() && endpoint.back() != '/') {
        endpoint += '/';
    }
    endpoint += "audio/transcriptions";

    std::string boundary = "----VoiceInputFormBoundary" + std::to_string(time(nullptr));
    std::string multipartBody = BuildMultipartBody(wavData, modelName_, language_, boundary);

    CURL* curl = curl_easy_init();
    if (!curl) {
        if (errorCb_) errorCb_("Failed to init curl");
        if (resultCb_) resultCb_("", true);
        return;
    }

    std::string contentType = "Content-Type: multipart/form-data; boundary=" + boundary;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, contentType.c_str());

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, multipartBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(multipartBody.size()));

    if (!apiKey_.empty()) {
        std::string authHeader = "Authorization: Bearer " + apiKey_;
        headers = curl_slist_append(headers, authHeader.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "fcitx5-voice-input/0.1.0");

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (state->cancelled) {
        FCITX_INFO() << "[voice-input:openai] Cancelled session=" << state->sessionId;
        return;
    }

    if (res != CURLE_OK) {
        FCITX_ERROR() << "[voice-input:openai] HTTP request failed: " << curl_easy_strerror(res);
        if (errorCb_) errorCb_("OpenAI request failed: " + std::string(curl_easy_strerror(res)));
        if (resultCb_) resultCb_("", true);
        return;
    }

    // Parse JSON response
    std::string text;
    try {
        Json::Value json;
        Json::Reader reader;
        if (reader.parse(response, json)) {
            text = json.get("text", json.get("error", Json::Value(""))).asString();
        }
    } catch (...) {
        // ignore parse errors
    }

    if (text.empty()) {
        FCITX_WARN() << "[voice-input:openai] Empty response";
        if (resultCb_) resultCb_("", true);
        return;
    }

    // Trim
    text.erase(0, text.find_first_not_of(" \t\n\r"));
    text.erase(text.find_last_not_of(" \t\n\r") + 1);

    FCITX_INFO() << "[voice-input:openai] final session=" << state->sessionId
                 << " \"" << text << "\"";
    if (resultCb_) resultCb_(text, true);
}

// ── OpenaiAsrEngine ──────────────────────────────────────────

bool OpenaiAsrEngine::Init(const Config& config) {
    config_ = config;
    return true;
}

std::shared_ptr<AsrSession> OpenaiAsrEngine::StartSession() {
    uint64_t sid;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sid = nextSessionId_++;
    }

    auto session = std::make_shared<OpenaiAsrSession>(
        config_, resultCb_, errorCb_, sid);

    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions_[sid] = session;
    }

    return session;
}

} // namespace fcitx
