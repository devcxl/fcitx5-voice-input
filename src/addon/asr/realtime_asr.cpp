#include "realtime_asr.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <thread>

#include <curl/curl.h>
#include <fcitx-utils/log.h>
#include <json/json.h>

#include "utils/base64.h"

using namespace std::chrono_literals;

namespace fcitx {
namespace {

constexpr int kSourceSampleRate = 16000;
constexpr int kTargetSampleRate = 24000;  // OpenAI Realtime 要求 >= 24kHz
constexpr int kAppendChunkSamples = 2400; // ~100ms @24k 音频推送粒度（含重采样后）

enum class RecvStatus { Ok, Again, Closed, Error };

// 16k → 24k 线性插值（比例 3/2）。输出样本数 = floor(1.5 * N)。
// 对每个输出下标 j，其对应的源位置 pos = j * 2/3，在相邻源样本间线性插值。
void Upsample16kTo24k(const std::vector<float>& in, std::vector<float>& out) {
    out.clear();
    if (in.empty()) return;
    size_t outCount = in.size() * 3 / 2;
    if (outCount == 0) return;
    out.reserve(outCount);
    for (size_t j = 0; j < outCount; ++j) {
        double pos = j * (2.0 / 3.0);
        size_t i = static_cast<size_t>(pos);
        double frac = pos - static_cast<double>(i);
        float a = in[i];
        float b = (i + 1 < in.size()) ? in[i + 1] : a;  // 末端 clamp
        out.push_back(a + static_cast<float>((b - a) * frac));
    }
}

std::string JsonToString(const Json::Value& json) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, json);
}

// 发送一个 TEXT WS 帧（OpenAI Realtime 用 JSON 文本帧）。
bool SendWebSocketText(CURL* curl, const std::string& data,
                       const std::atomic<bool>& cancelFlag) {
    size_t remaining = data.size();
    const char* ptr = data.data();
    while (remaining > 0) {
        if (cancelFlag.load()) return false;
        size_t sent = 0;
        CURLcode result =
            curl_ws_send(curl, ptr, remaining, &sent, 0, CURLWS_TEXT);
        if (result == CURLE_AGAIN) { std::this_thread::sleep_for(10ms); continue; }
        if (result != CURLE_OK) {
            FCITX_ERROR() << "[voice-input:realtime] curl_ws_send failed: "
                          << curl_easy_strerror(result);
            return false;
        }
        ptr += sent;
        remaining -= sent;
    }
    return true;
}

// 接收一帧 TEXT，返回 JSON 字符串。跨分片拼接（bytesleft）。
RecvStatus ReceiveTextFrame(CURL* curl, std::string& out) {
    out.clear();
    std::array<char, 8192> buffer{};
    while (true) {
        size_t received = 0;
        const struct curl_ws_frame* meta = nullptr;
        CURLcode result =
            curl_ws_recv(curl, buffer.data(), buffer.size(), &received, &meta);
        if (result == CURLE_AGAIN) return RecvStatus::Again;
        if (result != CURLE_OK) {
            FCITX_ERROR() << "[voice-input:realtime] curl_ws_recv failed: "
                          << curl_easy_strerror(result);
            return RecvStatus::Error;
        }
        if (meta && (meta->flags & CURLWS_CLOSE)) return RecvStatus::Closed;
        bool isText = meta && (meta->flags & CURLWS_TEXT);
        if (received > 0) out.append(buffer.data(), received);
        if (!meta || !isText) continue;
        if (meta->bytesleft == 0) return RecvStatus::Ok;
    }
}

void CloseWebSocket(CURL* curl) {
    if (!curl) return;
    size_t sent = 0;
    CURLcode res = curl_ws_send(curl, "", 0, &sent, 0, CURLWS_CLOSE);
    if (res != CURLE_OK)
        FCITX_WARN() << "[voice-input:realtime] WS close failed: "
                     << curl_easy_strerror(res);
}

} // namespace

// ── RealtimeAsrSession ────────────────────────────────────────

RealtimeAsrSession::RealtimeAsrSession(const AsrEngine::Config& config,
                                       AsrSession::ErrorCallback errorCb,
                                       uint64_t sessionId) {
    state_->sessionId = sessionId;
    errorCb_ = std::move(errorCb);
    audioChunks_ = std::make_shared<ThreadSafeQueue<std::vector<int16_t>>>();

    // 由 baseUrl（https://api.openai.com/v1）派生 wss 端点
    std::string base = config.apiEndpoint;
    if (!base.empty()) {
        const std::string httpsPrefix = "https://";
        const std::string wssPrefix = "wss://";
        if (base.compare(0, httpsPrefix.size(), httpsPrefix) == 0)
            base.replace(0, httpsPrefix.size(), wssPrefix);
        if (!base.empty() && base.back() != '/') base += '/';
    }
    modelName_ = config.modelName.empty() ? "gpt-live-transcribe" : config.modelName;
    endpoint_ = base + "realtime?model=" + modelName_;
    apiKey_ = config.apiKey;
    language_ = config.language;
    commitIntervalMs_ = std::clamp(config.commitIntervalMs, 1000, 30000);

    if (apiKey_.empty()) {
        state_->finished = true;
        if (errorCb_) errorCb_("OpenAI API key not configured");
        if (resultCb_) resultCb_("", true, state_->sessionId);
        return;
    }

    FCITX_DEBUG() << "[voice-input:realtime] Init session=" << sessionId
                  << " endpoint=" << endpoint_;
}

RealtimeAsrSession::~RealtimeAsrSession() {
    Cancel();
    JoinWithTimeout(5s);
}

void RealtimeAsrSession::StartWorker() {
    if (!state_->finished && !workerThread_) {
        auto self = std::static_pointer_cast<RealtimeAsrSession>(shared_from_this());
        workerThread_ = std::make_unique<std::thread>([self]() { self->WorkerLoop(); });
    }
}

void RealtimeAsrSession::FeedAudio(const float* pcm, size_t frames) {
    if (state_->cancelled || state_->finished) return;
    if (!audioChunks_) return;
    std::vector<int16_t> chunk(frames);
    for (size_t i = 0; i < frames; ++i) {
        float s = std::clamp(pcm[i], -1.0f, 1.0f);
        chunk[i] = static_cast<int16_t>(s * 32767.0f);
    }
    audioChunks_->Push(std::move(chunk));
}

void RealtimeAsrSession::End() {
    state_->finished = true;
    if (audioChunks_) audioChunks_->Push(std::vector<int16_t>());
}

void RealtimeAsrSession::Cancel() {
    state_->cancelled = true;
    if (audioChunks_) audioChunks_->Push(std::vector<int16_t>());
}

void RealtimeAsrSession::JoinWithTimeout(std::chrono::milliseconds timeout) {
    if (workerThread_ && workerThread_->joinable()) {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < timeout) {
            std::this_thread::sleep_for(10ms);
            if (!workerThread_->joinable()) { workerThread_->join(); return; }
        }
        FCITX_WARN() << "[voice-input:realtime] Join timeout session="
                     << state_->sessionId;
        workerThread_->detach();
    }
    workerThread_.reset();
}

void RealtimeAsrSession::WorkerLoop() {
    uint64_t sid = state_->sessionId;
    auto audioChunks = audioChunks_;  // keep queue alive
    auto cb = resultCb_;
    auto ecb = errorCb_;

    auto buildAppendEvent = [this](const std::vector<int16_t>& pcm24k) -> std::string {
        Json::Value ev;
        ev["type"] = "input_audio_buffer.append";
        std::vector<uint8_t> bytes(pcm24k.size() * 2);
        std::memcpy(bytes.data(), pcm24k.data(), bytes.size());
        ev["audio"] = Base64Encode(bytes.data(), bytes.size());
        return JsonToString(ev);
    };
    auto buildCommitEvent = []() -> std::string {
        Json::Value ev;
        ev["type"] = "input_audio_buffer.commit";
        return JsonToString(ev);
    };
    auto buildSessionUpdate = [this]() -> std::string {
        Json::Value ev;
        ev["type"] = "session.update";
        if (!language_.empty()) {
            ev["session"]["transcription"]["language"] = language_;
        }
        if (!modelName_.empty()) {
            ev["session"]["transcription"]["model"] = modelName_;
        }
        return JsonToString(ev);
    };

    bool gotFinal = false;
    bool endCommitSent = false;      // End commit 是否已发出（决定最终 item 判定）
    std::string fullTranscript;      // 会话级累积文本（整句，preedit 显示的依据）
    std::string currentTranscript;   // 当前 item 的 delta 累积
    std::vector<int16_t> pending24k;  // 提升到连接循环外，重连时保留未发送缓冲

    // 连接循环：处理首次连接 + 断线/30min 重连（保持同一 sessionId）
    for (int attempt = 0; attempt < kMaxReconnectAttempts && !gotFinal; ++attempt) {
        if (state_->cancelled) break;

        CURL* curl = curl_easy_init();
        if (!curl) {
            if (ecb) ecb("Failed to init curl");
            if (cb) cb("", true, sid);
            return;
        }

        struct curl_slist* headers = nullptr;
        std::string authHeader = "Authorization: Bearer " + apiKey_;
        headers = curl_slist_append(headers, authHeader.c_str());
        // GA 接口无需 OpenAI-Beta 头

        curl_easy_setopt(curl, CURLOPT_URL, endpoint_.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "fcitx5-voice-input/0.1.0");

        CURLcode connectResult = curl_easy_perform(curl);
        curl_slist_free_all(headers);

        if (state_->cancelled) {
            curl_easy_cleanup(curl);
            break;
        }
        if (connectResult != CURLE_OK) {
            FCITX_ERROR() << "[voice-input:realtime] WS connect failed: "
                          << curl_easy_strerror(connectResult) << " attempt=" << attempt;
            curl_easy_cleanup(curl);
            if (attempt == kMaxReconnectAttempts - 1) {
                if (ecb) ecb("Realtime connect failed: " + std::string(curl_easy_strerror(connectResult)));
                if (cb) cb("", true, sid);
                return;
            }
            std::this_thread::sleep_for(1s);
            continue;
        }

        FCITX_INFO() << "[voice-input:realtime] WS connected session=" << sid;

        // 会话开始：发送 session.update（语言/模型提示）
        SendWebSocketText(curl, buildSessionUpdate(), state_->cancelled);

        auto sessionStart = std::chrono::steady_clock::now();
        auto lastCommitTime = sessionStart;
        bool appendedSinceCommit = false;  // 自上次 commit 后是否 append 过音频
        int commitsInFlight = 0;           // 在途 commit 数（重连后新连接重新计数）

        // 处理服务端事件
        auto handleServer = [&](std::chrono::milliseconds timeout) {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!state_->cancelled && !gotFinal &&
                   std::chrono::steady_clock::now() < deadline) {
                std::string text;
                RecvStatus r = ReceiveTextFrame(curl, text);
                if (r == RecvStatus::Again) { std::this_thread::sleep_for(10ms); continue; }
                if (r == RecvStatus::Closed) { gotFinal = true; return false; }
                if (r == RecvStatus::Error) { if (ecb) ecb("WS recv error"); return false; }

                Json::Value json;
                Json::Reader reader;
                if (!reader.parse(text, json)) continue;

                std::string type = json.get("type", "").asString();
                if (type == "conversation.item.input_audio_transcription.delta") {
                    currentTranscript += json.get("delta", "").asString();
                    // preedit 显示整句累积：fullTranscript + 当前段增量
                    if (cb && !currentTranscript.empty()) {
                        std::string partial = fullTranscript + currentTranscript;
                        cb(partial, false, sid);
                    }
                } else if (type == "conversation.item.input_audio_transcription.completed") {
                    std::string transcript = json.get("transcript", "").asString();
                    // 判定这是否最终 item：End commit 已发出 且 在途 commit 只剩这一个
                    // （WS 事件有序保证旧周期 item 的 completed 先于最终 item 到达）
                    bool isFinalItem = endCommitSent && commitsInFlight <= 1;
                    if (commitsInFlight > 0) --commitsInFlight;
                    fullTranscript += transcript;
                    if (isFinalItem) {
                        // 最终结果（整句累积），上报 final 并结束会话
                        if (cb && !fullTranscript.empty()) cb(fullTranscript, true, sid);
                        currentTranscript.clear();
                        gotFinal = true;
                        return false;
                    } else {
                        // 周期 commit（或在途旧 item）产生的转录 → 累加后作为 partial 刷新 preedit
                        // （pipeline 契约：每个会话仅一个 final，此处不得上报 final）
                        if (cb && !fullTranscript.empty()) cb(fullTranscript, false, sid);
                        currentTranscript.clear();
                    }
                } else if (type == "conversation.item.input_audio_transcription.failed") {
                    if (ecb) ecb("Transcription failed");
                    currentTranscript.clear();
                }
                // 其他事件（response.*/error 等）忽略
            }
            return true;
        };

        bool reconnectNeeded = false;
        while (!state_->cancelled && !gotFinal && !reconnectNeeded) {
            std::vector<int16_t> chunk;
            bool hasChunk = audioChunks->TryPop(chunk);

            if (hasChunk && chunk.empty() && state_->finished) {
                // 语音结束：先把 pending24k 剩余音频全部 flush（不足 chunk 粒度也发，
                // 否则 commit 后尾部音频丢失），再提交最终段。
                // 语音已结束，任何发送失败/超时都不重连（重连后空 chunk 已消费、End 分支
                // 不会重试），直接以已累积文本兜底 final 退出。
                if (!pending24k.empty() && !state_->cancelled) {
                    if (!SendWebSocketText(curl, buildAppendEvent(pending24k), state_->cancelled)) {
                        FCITX_ERROR() << "[voice-input:realtime] End flush failed session=" << sid;
                        // 兜底 final 后经统一清理路径退出（break → CloseWebSocket + cleanup）
                        if (cb) cb(fullTranscript, true, sid);
                        gotFinal = true;
                        break;
                    }
                    pending24k.clear();
                }
                if (!SendWebSocketText(curl, buildCommitEvent(), state_->cancelled)) {
                    FCITX_ERROR() << "[voice-input:realtime] End commit failed session=" << sid;
                    if (cb) cb(fullTranscript, true, sid);
                    gotFinal = true;
                    break;
                }
                endCommitSent = true;
                ++commitsInFlight;
                auto deadline = std::chrono::steady_clock::now() + 30s;
                while (!state_->cancelled && !gotFinal &&
                       std::chrono::steady_clock::now() < deadline) {
                    if (!handleServer(50ms)) { gotFinal = true; break; }
                }
                if (!gotFinal && !state_->cancelled) {
                    // 30s 超时仍未收到最终 completed → 以已累积文本兜底 final，
                    // 并置 gotFinal 结束会话（否则 for 循环会重连后空转挂死）
                    FCITX_WARN() << "[voice-input:realtime] End wait timeout session=" << sid;
                    if (cb) cb(fullTranscript, true, sid);
                    gotFinal = true;
                }
                break;
            }
            if (hasChunk && !chunk.empty()) {
                // 16k int16 → 24k float → 24k int16
                std::vector<float> f16(chunk.size());
                for (size_t i = 0; i < chunk.size(); ++i)
                    f16[i] = static_cast<float>(chunk[i]) / 32768.0f;
                std::vector<float> f24;
                Upsample16kTo24k(f16, f24);
                std::vector<int16_t> i24(f24.size());
                for (size_t i = 0; i < f24.size(); ++i)
                    i24[i] = static_cast<int16_t>(std::clamp(f24[i], -1.0f, 1.0f) * 32767.0f);
                pending24k.insert(pending24k.end(), i24.begin(), i24.end());
            }

            // 周期 commit 兜底（长句无停顿也能出增量）
            // 条件：距上次 commit 超时 且 期间确实 append 过音频（服务端有待 commit 内容）
            auto now = std::chrono::steady_clock::now();
            auto elapsedSinceCommit = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastCommitTime).count();
            if (appendedSinceCommit && elapsedSinceCommit >= commitIntervalMs_) {
                if (!SendWebSocketText(curl, buildCommitEvent(), state_->cancelled)) {
                    reconnectNeeded = true;
                    break;
                }
                lastCommitTime = now;
                appendedSinceCommit = false;
                ++commitsInFlight;
            }

            // 推送足够粒度的音频
            if (pending24k.size() >= kAppendChunkSamples && !state_->cancelled) {
                size_t sendSize = (pending24k.size() / kAppendChunkSamples) * kAppendChunkSamples;
                std::vector<int16_t> toSend(pending24k.begin(), pending24k.begin() + sendSize);
                if (!SendWebSocketText(curl, buildAppendEvent(toSend), state_->cancelled)) {
                    reconnectNeeded = true;
                    break;
                }
                pending24k.erase(pending24k.begin(), pending24k.begin() + sendSize);
                appendedSinceCommit = true;
                // 注意：不要在此刷新 lastCommitTime。
                // lastCommitTime 的语义是「距上次 commit 的时间」，只在 commit 时更新；
                // 若在 append 后刷新，持续说话时周期 commit 将永远不会触发。
            }

            // 30min 会话上限：强制 commit + 重连（仅当有待 commit 音频时）
            auto sessionAge = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - sessionStart).count();
            if (sessionAge >= kSessionMaxDuration.count() && appendedSinceCommit) {
                FCITX_WARN() << "[voice-input:realtime] 30min session limit, reconnect";
                if (SendWebSocketText(curl, buildCommitEvent(), state_->cancelled)) {
                    ++commitsInFlight;
                } else {
                    FCITX_ERROR() << "[voice-input:realtime] 30min commit failed session=" << sid;
                }
                reconnectNeeded = true;
                break;
            }

            if (!handleServer(20ms)) { gotFinal = true; break; }
            std::this_thread::sleep_for(5ms);
        }

        CloseWebSocket(curl);
        curl_easy_cleanup(curl);

        if (state_->cancelled || gotFinal) break;
        if (reconnectNeeded) {
            FCITX_WARN() << "[voice-input:realtime] Reconnecting session=" << sid;
            // 重连后是全新服务端会话，旧 item 的 delta 上下文已失效：
            // 清掉当前 item 半截累积，避免 preedit 显示幽灵残文（fullTranscript 保留）
            currentTranscript.clear();
            std::this_thread::sleep_for(1s);
            continue;
        }
    }

    if (state_->cancelled) {
        FCITX_DEBUG() << "[voice-input:realtime] Cancelled session=" << sid;
        return;
    }
    if (!gotFinal) {
        // 未收到最终转录（连接断开/失败）：以空 final 触发 pipeline 错误/清理分支
        if (cb) cb("", true, sid);
    }
}

// ── RealtimeAsrEngine ─────────────────────────────────────────

bool RealtimeAsrEngine::Init(const Config& config) {
    config_ = config;
    return true;
}

std::shared_ptr<AsrSession> RealtimeAsrEngine::StartSession() {
    uint64_t sid;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        // 清理过期 weak_ptr，避免长期运行 map 无限增长
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            if (it->second.expired()) it = sessions_.erase(it);
            else ++it;
        }
        sid = nextSessionId_++;
    }

    auto session = std::make_shared<RealtimeAsrSession>(config_, errorCb_, sid);
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
