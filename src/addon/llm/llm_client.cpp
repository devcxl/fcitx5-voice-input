#include "llm_client.h"

#include <chrono>
#include <cstring>
#include <string>

#include <curl/curl.h>
#include <json/json.h>

#include <fcitx-utils/log.h>

namespace fcitx {

namespace {

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* response = static_cast<std::string*>(userp);
    size_t total = size * nmemb;
    response->append(static_cast<char*>(contents), total);
    return total;
}

// Extract "text" field from a JSON response.
// Returns empty string on parse failure or missing field.
std::string ExtractJsonText(const std::string& content) {
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(content, root)) {
        FCITX_WARN() << "[voice-input:llm] Response is not valid JSON";
        return {};
    }
    if (!root.isMember("text") || !root["text"].isString()) {
        FCITX_WARN() << "[voice-input:llm] JSON response missing 'text' field";
        return {};
    }
    return root["text"].asString();
}

// Build system prompt with JSON format instruction appended.
std::string BuildSystemPrompt(const std::string& userPrompt) {
    std::string prompt = userPrompt;
    if (!prompt.empty()) {
        prompt += "\n\n";
    }
    prompt += "你必须以JSON格式回复，格式为：{\"text\": \"修正后的文本\"}。";
    return prompt;
}

// SSE streaming callback
struct SSEContext {
    std::string buffer;
    std::function<void(const std::string&)> onToken;
    std::function<void(const std::string&)> onComplete;
    std::string accumulated;
    bool done = false;
};

size_t SSEWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* ctx = static_cast<SSEContext*>(userp);
    size_t total = size * nmemb;
    ctx->buffer.append(static_cast<char*>(contents), total);

    // Parse complete SSE events (separated by "\n\n")
    size_t pos;
    while (!ctx->done && (pos = ctx->buffer.find("\n\n")) != std::string::npos) {
        std::string event = ctx->buffer.substr(0, pos);
        ctx->buffer.erase(0, pos + 2);

        // Parse "data: {...}" or "data:[DONE]"
        const char* prefix = "data:";
        if (event.compare(0, 5, prefix) != 0) continue;

        std::string data = event.substr(5);
        // Trim leading space
        if (!data.empty() && data[0] == ' ') data.erase(0, 1);

        if (data == "[DONE]") {
            ctx->done = true;
            ctx->onComplete(ctx->accumulated);
            continue;
        }

        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(data, root)) continue;

        std::string content = root["choices"][0]["delta"]["content"].asString();
        if (content.empty()) continue;

        ctx->accumulated += content;
        ctx->onToken(ctx->accumulated);
    }

    return total;
}

} // namespace

LLMClient::LLMClient(Config config)
    : config_(std::move(config)) {}

LLMClient::~LLMClient() = default;

std::string LLMClient::Process(const std::string& text) {
    if (config_.model.empty()) {
        return text;
    }

    // Build URL
    std::string url = config_.endpoint;
    if (!url.empty() && url.back() != '/') {
        url += '/';
    }
    url += "chat/completions";

    // Build JSON body
    Json::Value body;
    body["model"] = config_.model;
    body["temperature"] = 0.1;

    Json::Value messages(Json::arrayValue);

    {
        Json::Value sysMsg;
        sysMsg["role"] = "system";
        sysMsg["content"] = BuildSystemPrompt(config_.systemPrompt);
        messages.append(sysMsg);
    }

    Json::Value userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = text;
    messages.append(userMsg);

    body["messages"] = messages;

    Json::StreamWriterBuilder writer;
    std::string bodyStr = Json::writeString(writer, body);

    FCITX_DEBUG() << "[voice-input:llm] POST " << url
                 << " model=" << config_.model
                 << " input=" << text.size() << " chars"
                 << " body=" << bodyStr.size() << " bytes";

    FCITX_DEBUG() << "[voice-input:llm] Request body:\n" << bodyStr;

    // HTTP request
    auto tStart = std::chrono::steady_clock::now();

    CURL* curl = curl_easy_init();
    if (!curl) {
        FCITX_ERROR() << "[voice-input:llm] Failed to init curl";
        return {};
    }

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string authHeader = "Authorization: Bearer " + config_.apiKey;
    headers = curl_slist_append(headers, authHeader.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, bodyStr.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "fcitx5-voice-input/0.1.0");

    CURLcode res = curl_easy_perform(curl);

    long httpCode = 0;
    long osErrno = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &osErrno);

    auto tEnd = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    FCITX_DEBUG() << "[voice-input:llm] HTTP " << httpCode
                  << " response=" << response.size() << " bytes"
                  << " elapsed=" << elapsedMs << "ms";

    if (res != CURLE_OK || httpCode != 200) {
        std::string curlErr = curl_easy_strerror(res);
        const char* diagnostic = "";
        switch (res) {
        case CURLE_COULDNT_RESOLVE_HOST: diagnostic = " (DNS resolution failed)"; break;
        case CURLE_COULDNT_CONNECT:      diagnostic = " (TCP connect failed, check endpoint/firewall)"; break;
        case CURLE_SSL_CONNECT_ERROR:    diagnostic = " (SSL handshake failed)"; break;
        case CURLE_OPERATION_TIMEDOUT:   diagnostic = " (connection timed out, check network)"; break;
        case CURLE_URL_MALFORMAT:        diagnostic = " (malformed URL)"; break;
        case CURLE_HTTP_RETURNED_ERROR:  diagnostic = " (HTTP error response)"; break;
        default: break;
        }
        FCITX_WARN() << "[voice-input:llm] Request failed: "
                     << curlErr << diagnostic
                     << " http=" << httpCode
                     << " elapsed=" << elapsedMs << "ms"
                     << " osErrno=" << osErrno;
        return {};
    }

    // Parse response
    Json::Value json;
    Json::Reader reader;
    if (!reader.parse(response, json)) {
        FCITX_WARN() << "[voice-input:llm] JSON parse failed"
                     << " elapsed=" << elapsedMs << "ms"
                     << " responseHead=" << response.substr(0, 200);
        return {};
    }

    std::string content = json["choices"][0]["message"]["content"].asString();
    if (content.empty()) {
        FCITX_WARN() << "[voice-input:llm] Empty response content"
                     << " elapsed=" << elapsedMs << "ms";
        return {};
    }

    // Try JSON extraction, fallback to raw LLM output
    std::string extracted = ExtractJsonText(content);
    std::string result = extracted.empty() ? text : extracted;

    FCITX_DEBUG() << "[voice-input:llm] Response: http=" << httpCode
                 << " elapsed=" << elapsedMs << "ms"
                 << " output=" << result.size() << " chars";

    FCITX_DEBUG() << "[voice-input:llm] Response body:\n" << response;

    FCITX_DEBUG() << "[voice-input:llm] Done: raw=\"" << text
                 << "\" → out=\"" << result << "\"";
    return result;
}

void LLMClient::ProcessStream(const std::string& text,
                               std::function<void(const std::string&)> onToken,
                               std::function<void(const std::string&)> onComplete) {
    if (config_.model.empty() || !onComplete) return;

    std::string url = config_.endpoint;
    if (!url.empty() && url.back() != '/') {
        url += '/';
    }
    url += "chat/completions";

    Json::Value body;
    body["model"] = config_.model;
    body["temperature"] = 0.1;
    body["stream"] = true;

    Json::Value messages(Json::arrayValue);

    {
        Json::Value sysMsg;
        sysMsg["role"] = "system";
        sysMsg["content"] = BuildSystemPrompt(config_.systemPrompt);
        messages.append(sysMsg);
    }

    Json::Value userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = text;
    messages.append(userMsg);

    body["messages"] = messages;

    Json::StreamWriterBuilder writer;
    std::string bodyStr = Json::writeString(writer, body);

    FCITX_DEBUG() << "[voice-input:llm:stream] POST " << url
                 << " model=" << config_.model
                 << " input=" << text.size() << " chars";

    auto tStart = std::chrono::steady_clock::now();

    CURL* curl = curl_easy_init();
    if (!curl) {
        FCITX_ERROR() << "[voice-input:llm:stream] Failed to init curl";
        return;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string authHeader = "Authorization: Bearer " + config_.apiKey;
    headers = curl_slist_append(headers, authHeader.c_str());

    SSEContext ctx;
    // Skip onToken during streaming — partial JSON is not user-friendly
    ctx.onToken = [](const std::string&) {};
    ctx.onComplete = [&onComplete, text](const std::string& s) {
        if (!onComplete) return;
        std::string extracted = ExtractJsonText(s);
        if (!extracted.empty()) {
            onComplete(extracted);
        } else {
            FCITX_WARN() << "[voice-input:llm:stream] Failed to parse JSON, "
                         << "falling back to raw ASR text";
            onComplete(text);
        }
    };

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, bodyStr.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, SSEWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "fcitx5-voice-input/0.1.0");

    CURLcode res = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    auto tEnd = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || httpCode != 200) {
        FCITX_WARN() << "[voice-input:llm:stream] Request failed: "
                     << curl_easy_strerror(res)
                     << " http=" << httpCode
                     << " elapsed=" << elapsedMs << "ms";
        return;
    }

    FCITX_DEBUG() << "[voice-input:llm:stream] Done: elapsed=" << elapsedMs << "ms"
                 << " accumulated=" << ctx.accumulated.size() << " chars"
                 << " \"" << ctx.accumulated << "\"";
}

} // namespace fcitx
