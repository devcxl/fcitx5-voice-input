#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "asr_session.h"

namespace fcitx {

struct AsrSessionStart {
    std::shared_ptr<AsrSession> session;
    std::optional<uint64_t> cancelledSessionId;
};

/// 本地后端已加载模型的清单身份。
///
/// 由后端在 Init() 成功后缓存并返回；云端后端、未成功 Init 或无法确定时
/// valid=false。调用方（状态镜像、GUI）不得为空值编造版本。
struct AsrModelIdentity {
    bool valid = false;
    std::string id;       // 清单 id，例如 "online-zipformer"
    std::string version;  // 不可变版本目录名
};

/// ASR 后端引擎，全局单例（由 Pipeline 持有）。
/// 工厂模式：StartSession() 返回独立会话及容量取消结果。
/// 内部用 weak_ptr 追踪活跃 Session，用于批量取消。
class AsrEngine {
public:
    static constexpr size_t kMaxActiveSessions = 3;

    struct Config {
        // Common
        std::string modelName;

        // Sherpa-onnx / Zipformer (local)
        std::string hotwordsFile;
        float hotwordsScore = 1.5f;
        int numThreads = 2;

        // OpenAI-compatible (cloud)
        std::string apiEndpoint;
        std::string apiKey;
        std::string language = "zh";
        std::string apiMode = "whisper";     // "whisper" or "chat" or "realtime"

        // Chat Completions 模式：DashScope 扩展字段 asr_options.enable_itn
        bool enableItn = true;

        // OpenAI Realtime (streaming): periodic commit interval for long speech
        int commitIntervalMs = 5000;

        // Mistral Realtime: target streaming delay (ms) to trade latency for accuracy
        int targetStreamingDelayMs = 0;

        // Volcengine Doubao streaming ASR (cloud)
        std::string authMode;
        std::string appKey;
        std::string accessKey;
        std::string resourceId;
        int chunkMs = 200;
        bool enableItN = true;
        bool enablePunc = true;
        bool enableDdc = false;
        bool enableNonstream = true;
        int endWindowMs = 800;
    };

    virtual ~AsrEngine();

    /// 初始化引擎，加载配置。可多次调用以热更新。
    /// 调用时若有活跃 Session，调用方应先 CancelAllSessions。
    virtual bool Init(const Config& config) = 0;

    /// 创建新识别会话；若容量已满，同时返回实际取消的旧会话 ID。
    /// 调用方完成 session 映射和回调登记后再启动 worker。
    virtual AsrSessionStart StartSession() = 0;

    /// 取消所有由本 Engine 创建的活跃 Session。
    virtual void CancelAllSessions();

    virtual const char* Name() const = 0;

    /// 已加载模型的身份（仅本地后端有意义）。
    /// 未成功 Init、云端后端或无法确定时返回 valid=false；调用方不得据此编造版本。
    virtual AsrModelIdentity ModelIdentity() const { return {}; }

    void SetResultCallback(AsrSession::ResultCallback cb) { resultCb_ = std::move(cb); }
    void SetErrorCallback(AsrSession::ErrorCallback cb) { errorCb_ = std::move(cb); }

protected:
    static uint64_t NextSessionId() {
        return nextSessionId_.fetch_add(1, std::memory_order_relaxed);
    }

    std::optional<uint64_t> CancelOldestSessionIfLimitReachedLocked() {
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (it->second.expired()) {
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
        if (sessions_.size() < maxActiveSessions_) {
            return std::nullopt;
        }

        auto oldest = sessions_.begin();
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            auto session = it->second.lock();
            auto oldestSession = oldest->second.lock();
            if (session && (!oldestSession ||
                            session->GetState()->sessionId <
                                oldestSession->GetState()->sessionId)) {
                oldest = it;
            }
        }

        const auto sessionId = oldest->first;
        if (auto session = oldest->second.lock()) {
            session->Cancel();
        }
        sessions_.erase(oldest);
        return sessionId;
    }

    AsrSession::ResultCallback resultCb_;
    AsrSession::ErrorCallback errorCb_;

    std::mutex sessionsMutex_;
    std::unordered_map<uint64_t, std::weak_ptr<AsrSession>> sessions_;
    size_t maxActiveSessions_{kMaxActiveSessions};

private:
    inline static std::atomic<uint64_t> nextSessionId_{1};
};

} // namespace fcitx
