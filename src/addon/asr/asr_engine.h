#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "asr_session.h"

namespace fcitx {

/// ASR 后端引擎，全局单例（由 Pipeline 持有）。
/// 工厂模式：StartSession() 返回独立会话对象。
/// 内部用 weak_ptr 追踪活跃 Session，用于批量取消。
class AsrEngine {
public:
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
        std::string apiMode = "whisper";     // "whisper" or "chat"

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

    /// 创建新识别会话。返回 shared_ptr。
    /// 超过 maxActiveSessions 时自动取消最旧会话。
    /// 若旧会话未结束，内部调用 Cancel（不阻塞）。
    virtual std::shared_ptr<AsrSession> StartSession() = 0;

    /// 取消所有由本 Engine 创建的活跃 Session。
    virtual void CancelAllSessions();

    virtual const char* Name() const = 0;

    void SetResultCallback(AsrSession::ResultCallback cb) { resultCb_ = std::move(cb); }
    void SetErrorCallback(AsrSession::ErrorCallback cb) { errorCb_ = std::move(cb); }

protected:
    AsrSession::ResultCallback resultCb_;
    AsrSession::ErrorCallback errorCb_;

    std::mutex sessionsMutex_;
    std::unordered_map<uint64_t, std::weak_ptr<AsrSession>> sessions_;
    uint64_t nextSessionId_{1};
    size_t maxActiveSessions_{3};
};

} // namespace fcitx
