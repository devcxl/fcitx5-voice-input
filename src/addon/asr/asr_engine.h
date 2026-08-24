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

/// ASR 后端引擎，全局单例（由 Pipeline 持有）。
/// 工厂模式：StartSession() 返回独立会话及容量取消结果。
/// 内部用 weak_ptr 追踪活跃 Session，用于批量取消。
class AsrEngine {
public:
    static constexpr size_t kMaxActiveSessions = 3;

    struct Config {
        // Common
        std::string modelName;

        // Sherpa-onnx (local)
        std::string modelPath;
        int numThreads = 4;

        // OpenAI-compatible (cloud)
        std::string apiEndpoint;
        std::string apiKey;
        std::string language = "zh";
        std::string apiMode = "whisper";     // "whisper" or "chat" or "realtime"

        // OpenAI Realtime (streaming): periodic commit interval for long speech
        int commitIntervalMs = 5000;

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
