#include "mistral_asr.h"

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

constexpr int kAppendChunkSamples = 1600; // ~100ms @16k 音频推送粒度
constexpr size_t kMaxFrameBytes = 16 * 1024 * 1024; // 单帧 16MB 上限

enum class RecvStatus { Ok, Again, Closed, Error };

std::string JsonToString(const Json::Value& json) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, json);
}

int CancelProgressCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t,
                           curl_off_t) {
    auto* cancelled = static_cast<std::atomic<bool>*>(clientp);
    return cancelled->load(std::memory_order_acquire) ? 1 : 0;
}

// 发送一个 TEXT WS 帧（Mistral Realtime 用 JSON 文本帧）。
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
            FCITX_ERROR() << "[voice-input:mistral] curl_ws_send failed: "
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
        // curl >= 8.2.0 将 curl_ws_recv 的 metap 参数 const 化；老版本（Debian 12 的
        // 7.88 等）为非 const 签名，需按编译期 curl 版本分支避免 -fpermissive 错误
#if LIBCURL_VERSION_NUM >= 0x080200
        const struct curl_ws_frame* meta = nullptr;
#else
        struct curl_ws_frame* meta = nullptr;
#endif
        CURLcode result =
            curl_ws_recv(curl, buffer.data(), buffer.size(), &received, &meta);
        if (result == CURLE_AGAIN) return RecvStatus::Again;
        if (result != CURLE_OK) {
            FCITX_ERROR() << "[voice-input:mistral] curl_ws_recv failed: "
                          << curl_easy_strerror(result);
            return RecvStatus::Error;
        }
        if (meta && (meta->flags & CURLWS_CLOSE)) return RecvStatus::Closed;
        bool isText = meta && (meta->flags & CURLWS_TEXT);
        if (received > 0) {
            // 限制单帧累计大小，防止恶意/异常服务端耗尽内存
            if (out.size() + received > kMaxFrameBytes) {
                FCITX_ERROR() << "[voice-input:mistral] WS frame exceeds "
                              << kMaxFrameBytes << " bytes";
                return RecvStatus::Error;
            }
            out.append(buffer.data(), received);
        }
        if (!meta || !isText) continue;
        if (meta->bytesleft == 0) return RecvStatus::Ok;
    }
}

void CloseWebSocket(CURL* curl) {
    if (!curl) return;
    size_t sent = 0;
    CURLcode res = curl_ws_send(curl, "", 0, &sent, 0, CURLWS_CLOSE);
    if (res != CURLE_OK)
        FCITX_WARN() << "[voice-input:mistral] WS close failed: "
                     << curl_easy_strerror(res);
}

} // namespace

// ── MistralAsrSession ────────────────────────────────────────

MistralAsrSession::MistralAsrSession(const AsrEngine::Config& config,
                                     AsrSession::ErrorCallback errorCb,
                                     uint64_t sessionId) {
    state_->sessionId = sessionId;
    errorCb_ = std::move(errorCb);
    audioChunks_ = std::make_shared<ThreadSafeQueue<std::vector<int16_t>>>(512);

    // Mistral Realtime 端点固定为官方地址（自托管需走代理/网关时可在配置中改 baseUrl）
    std::string base = config.apiEndpoint;
    if (base.empty()) base = "https://api.mistral.ai/v1";
    const std::string httpsPrefix = "https://";
    const std::string wssPrefix = "wss://";
    if (base.compare(0, httpsPrefix.size(), httpsPrefix) == 0)
        base.replace(0, httpsPrefix.size(), wssPrefix);
    if (!base.empty() && base.back() != '/') base += '/';

    modelName_ = config.modelName.empty()
                     ? "voxtral-mini-transcribe-realtime-2602"
                     : config.modelName;
    // base 形如 wss://api.mistral.ai/v1/
    endpoint_ = base + "audio/transcriptions/realtime?model=" + modelName_;
    apiKey_ = config.apiKey;
    commitIntervalMs_ = std::clamp(config.commitIntervalMs, 1000, 30000);
    targetStreamingDelayMs_ = std::clamp(config.targetStreamingDelayMs, 0, 1000);

    if (apiKey_.empty()) {
        state_->finished = true;
        if (errorCb_) errorCb_("Mistral API key not configured");
        if (resultCb_) resultCb_("", true, state_->sessionId);
        return;
    }

    FCITX_DEBUG() << "[voice-input:mistral] Init session=" << sessionId
                  << " endpoint=" << endpoint_;
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
            self->WorkerLoop();
            self->state_->workerDone.store(true, std::memory_order_release);
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
            // worker 持有最后一个会话引用时，析构会在自身线程执行，不能 join 自身。
            workerThread_->detach();
        } else if (WaitForWorkerCompletion(state_->workerDone, timeout)) {
            workerThread_->join();
        } else {
            FCITX_WARN() << "[voice-input:mistral] Join timeout session="
                         << state_->sessionId;
            workerThread_->detach();
        }
    }
    workerThread_.reset();
}

void MistralAsrSession::WorkerLoop() {
    uint64_t sid = state_->sessionId;
    auto audioChunks = audioChunks_;  // keep queue alive
    auto cb = resultCb_;
    auto ecb = errorCb_;

    auto buildAppendEvent = [this](const std::vector<int16_t>& pcm) -> std::string {
        Json::Value ev;
        ev["type"] = "input_audio.append";
        std::vector<uint8_t> bytes(pcm.size() * 2);
        std::memcpy(bytes.data(), pcm.data(), bytes.size());
        ev["audio"] = Base64Encode(bytes.data(), bytes.size());
        return JsonToString(ev);
    };
    auto buildFlushEvent = []() -> std::string {
        Json::Value ev;
        ev["type"] = "input_audio.flush";
        return JsonToString(ev);
    };
    auto buildEndEvent = []() -> std::string {
        Json::Value ev;
        ev["type"] = "input_audio.end";
        return JsonToString(ev);
    };
    auto buildSessionUpdate = [this]() -> std::string {
        Json::Value ev;
        ev["type"] = "session.update";
        // 音频格式：Mistral Realtime 原生接受 16k s16le PCM
        ev["session"]["audio_format"]["encoding"] = "pcm_s16le";
        ev["session"]["audio_format"]["sample_rate"] = 16000;
        // 目标流式延迟：等更多上下文换取同音字/准确率（0 = 不等待，最快）
        ev["session"]["target_streaming_delay_ms"] = targetStreamingDelayMs_;
        return JsonToString(ev);
    };

    bool gotDone = false;         // 是否已收到 transcription.done（最终终止）
    bool endSent = false;         // input_audio.end 是否已发出
    std::string fullTranscript;   // 会话级累积文本（整句，preedit 显示的依据）
    std::string currentTranscript; // 当前段 delta 累积
    std::vector<int16_t> pending16k; // 提升到连接循环外，重连时保留未发送缓冲

    // 连接循环：处理首次连接 + 断线/30min 重连（保持同一 sessionId）
    for (int attempt = 0;
         attempt < kMaxReconnectAttempts && !gotDone; ++attempt) {
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

        curl_easy_setopt(curl, CURLOPT_URL, endpoint_.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "fcitx5-voice-input/0.1.0");
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CancelProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state_->cancelled);

        CURLcode connectResult = curl_easy_perform(curl);
        curl_slist_free_all(headers);

        if (state_->cancelled) {
            curl_easy_cleanup(curl);
            break;
        }
        if (connectResult != CURLE_OK) {
            FCITX_ERROR() << "[voice-input:mistral] WS connect failed: "
                          << curl_easy_strerror(connectResult) << " attempt=" << attempt;
            curl_easy_cleanup(curl);
            if (attempt == kMaxReconnectAttempts - 1) {
                if (ecb) ecb("Mistral connect failed: " + std::string(curl_easy_strerror(connectResult)));
                if (cb) cb("", true, sid);
                return;
            }
            std::this_thread::sleep_for(1s);
            continue;
        }

        FCITX_INFO() << "[voice-input:mistral] WS connected session=" << sid;

        // 会话开始：发送 session.update（音频格式/语言提示）
        SendWebSocketText(curl, buildSessionUpdate(), state_->cancelled);

        auto sessionStart = std::chrono::steady_clock::now();
        auto lastFlushTime = sessionStart;
        bool appendedSinceFlush = false; // 自上次 flush 后是否 append 过音频

        // 处理服务端事件；返回 false 表示传输失败（需重连/结束）
        auto handleServer = [&](std::chrono::milliseconds timeout) -> bool {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!state_->cancelled && !gotDone &&
                   std::chrono::steady_clock::now() < deadline) {
                std::string text;
                RecvStatus r = ReceiveTextFrame(curl, text);
                if (r == RecvStatus::Again) { std::this_thread::sleep_for(10ms); continue; }
                if (r == RecvStatus::Closed) return false;
                if (r == RecvStatus::Error) {
                    if (ecb) ecb("WS recv error");
                    return false;
                }

                Json::Value json;
                Json::Reader reader;
                if (!reader.parse(text, json)) continue;

                std::string type = json.get("type", "").asString();
                if (type == "transcription.text.delta") {
                    currentTranscript += json.get("text", "").asString();
                    // preedit 显示整句累积：fullTranscript + 当前段增量
                    if (cb && !currentTranscript.empty()) {
                        std::string partial = fullTranscript + currentTranscript;
                        cb(partial, false, sid);
                    }
                } else if (type == "transcription.segment") {
                    // 服务端返回一个已完成的段（类似 GPT 的 completed）。
                    // Mistral 的 segment 事件携带完整段落文本；若与 delta 累积
                    // 语义重叠，则以 delta 累积为准，此处仅做完整性兜底。
                    std::string seg = json.get("text", "").asString();
                    if (seg.empty() && json.isMember("segments")) {
                        const auto& segs = json["segments"];
                        if (segs.isArray() && segs.size() > 0) {
                            seg = segs[0].get("text", "").asString();
                        }
                    }
                    if (!seg.empty()) {
                        fullTranscript += seg;
                        currentTranscript.clear();
                        if (cb) cb(fullTranscript, false, sid);
                    }
                } else if (type == "transcription.done") {
                    // 流终止：服务端已处理完所有音频，返回最终文本（也可能为空）。
                    // 若服务端未单独发 segment，则 done 的 text 是最终完整结果。
                    std::string finalText = json.get("text", "").asString();
                    if (!finalText.empty() && fullTranscript.find(finalText) == std::string::npos) {
                        fullTranscript += finalText;
                    }
                    gotDone = true;
                    if (cb && !fullTranscript.empty()) cb(fullTranscript, true, sid);
                    else if (cb) cb("", true, sid);
                    return true;
                } else if (type == "error") {
                    std::string msg = json.get("message", "").asString();
                    if (msg.empty()) msg = "Mistral realtime error";
                    if (ecb) ecb(msg);
                    // 错误后服务端可能直接关闭；若未关闭则继续等待 done/关闭
                    FCITX_WARN() << "[voice-input:mistral] server error: " << msg
                                 << " session=" << sid;
                }
                // 其他事件（session.created/updated 等）忽略
            }
            return true;
        };

        bool reconnectNeeded = false;
        while (!state_->cancelled && !gotDone && !reconnectNeeded) {
            std::vector<int16_t> chunk;
            bool hasChunk = audioChunks->TryPop(chunk);

            if (hasChunk && chunk.empty() && state_->finished) {
                // 语音结束：先把 pending16k 剩余音频全部 flush，再发 end 终止流。
                // 结束后任何发送失败/超时都不重连（空 chunk 已消费、End 分支不会重试），
                // 直接以已累积文本兜底 final 退出。
                if (!pending16k.empty() && !state_->cancelled) {
                    if (!SendWebSocketText(curl, buildAppendEvent(pending16k), state_->cancelled)) {
                        FCITX_ERROR() << "[voice-input:mistral] End flush failed session=" << sid;
                        if (cb) cb(fullTranscript, true, sid);
                        gotDone = true;
                        break;
                    }
                    pending16k.clear();
                }
                if (!SendWebSocketText(curl, buildEndEvent(), state_->cancelled)) {
                    FCITX_ERROR() << "[voice-input:mistral] End send failed session=" << sid;
                    if (cb) cb(fullTranscript, true, sid);
                    gotDone = true;
                    break;
                }
                endSent = true;
                auto deadline = std::chrono::steady_clock::now() + 30s;
                while (!state_->cancelled && !gotDone &&
                       std::chrono::steady_clock::now() < deadline) {
                    if (!handleServer(50ms)) { gotDone = true; break; }
                }
                if (!gotDone && !state_->cancelled) {
                    // 30s 超时仍未收到 done → 以已累积文本兜底 final，结束会话。
                    FCITX_WARN() << "[voice-input:mistral] End wait timeout session=" << sid;
                    if (cb) cb(fullTranscript, true, sid);
                    gotDone = true;
                }
                break;
            }
            if (hasChunk && !chunk.empty()) {
                // Mistral Realtime 原生接受 16k s16le PCM，无需重采样，直接累积。
                pending16k.insert(pending16k.end(), chunk.begin(), chunk.end());
            }

            // 周期 flush 兜底（长句无停顿也能出增量）
            // 条件：距上次 flush 超时 且 期间确实 append 过音频（服务端有待 flush 内容）
            auto now = std::chrono::steady_clock::now();
            auto elapsedSinceFlush = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastFlushTime).count();
            if (appendedSinceFlush && elapsedSinceFlush >= commitIntervalMs_) {
                if (!SendWebSocketText(curl, buildFlushEvent(), state_->cancelled)) {
                    reconnectNeeded = true;
                    break;
                }
                lastFlushTime = now;
                appendedSinceFlush = false;
            }

            // 推送足够粒度的音频
            if (pending16k.size() >= kAppendChunkSamples && !state_->cancelled) {
                size_t sendSize = (pending16k.size() / kAppendChunkSamples) * kAppendChunkSamples;
                std::vector<int16_t> toSend(pending16k.begin(), pending16k.begin() + sendSize);
                if (!SendWebSocketText(curl, buildAppendEvent(toSend), state_->cancelled)) {
                    reconnectNeeded = true;
                    break;
                }
                pending16k.erase(pending16k.begin(), pending16k.begin() + sendSize);
                appendedSinceFlush = true;
                // 注意：不要在此刷新 lastFlushTime。
                // lastFlushTime 的语义是「距上次 flush 的时间」，只在 flush 时更新；
                // 若在 append 后刷新，持续说话时周期 flush 将永远不会触发。
            }

            // 30min 会话上限：强制 flush + 重连（仅当有待 flush 音频时）
            auto sessionAge = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - sessionStart).count();
            if (sessionAge >= kSessionMaxDuration.count() && appendedSinceFlush) {
                FCITX_WARN() << "[voice-input:mistral] 30min session limit, reconnect";
                if (SendWebSocketText(curl, buildFlushEvent(), state_->cancelled)) {
                    appendedSinceFlush = false;
                } else {
                    FCITX_ERROR() << "[voice-input:mistral] 30min flush failed session=" << sid;
                }
                reconnectNeeded = true;
                break;
            }

            if (!handleServer(20ms)) { gotDone = true; break; }
            std::this_thread::sleep_for(5ms);
        }

        CloseWebSocket(curl);
        curl_easy_cleanup(curl);

        if (state_->cancelled || gotDone) break;
        if (reconnectNeeded) {
            FCITX_WARN() << "[voice-input:mistral] Reconnecting session=" << sid;
            // 重连后是全新服务端会话，旧段 delta 上下文已失效：
            // 清掉当前段半截累积，避免 preedit 显示幽灵残文（fullTranscript 保留）
            currentTranscript.clear();
            std::this_thread::sleep_for(1s);
            continue;
        }
    }

    if (state_->cancelled) {
        FCITX_DEBUG() << "[voice-input:mistral] Cancelled session=" << sid;
        return;
    }
    if (!gotDone && !endSent) {
        // 未正常结束（连接断开/失败）：以空 final 触发 pipeline 错误/清理分支。
        // endSent 已发时说明已走 End 兜底分支，不必重复上报。
        if (cb) cb("", true, sid);
    }
}

// ── MistralAsrEngine ─────────────────────────────────────────

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
