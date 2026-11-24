#include "engine_backend.h"

#include <utility>

#include <fcitx-utils/log.h>

#include "asr/mistral_asr.h"
#include "asr/openai_asr.h"
#include "asr/realtime_asr.h"
#include "asr/sherpa_offline_asr.h"
#include "asr/sherpa_onnx_asr.h"
#include "asr/volcengine_asr.h"
#include "engine.h"

namespace fcitx {

namespace {


// 端点走明文协议且非本机回环时，API Key 将明文传输，给出醒目警告。
bool EndpointUsesPlaintext(const std::string& endpoint) {
    if (endpoint.empty()) return false;
    if (endpoint.rfind("https://", 0) == 0 || endpoint.rfind("wss://", 0) == 0)
        return false;
    if (endpoint.rfind("http://", 0) == 0 || endpoint.rfind("ws://", 0) == 0) {
        if (endpoint.find("localhost") != std::string::npos ||
            endpoint.find("127.0.0.1") != std::string::npos ||
            endpoint.find("[::1]") != std::string::npos)
            return false;
        return true;
    }
    return false;
}


} // namespace

bool IsLocalAsrBackend(const std::string& backend) {
    return backend == "sherpa_onnx" || backend == "sherpa_offline";
}

std::unique_ptr<AsrEngine> CreateAsrEngine(const AsrBackendSnapshot& snapshot) {
    std::unique_ptr<AsrEngine> engine;
    switch (snapshot.kind) {
    case AsrEngineKind::Volcengine:
        engine = std::make_unique<VolcengineAsrEngine>();
        break;
    case AsrEngineKind::Mistral:
        engine = std::make_unique<MistralAsrEngine>();
        break;
    case AsrEngineKind::SherpaOnnx:
        engine = std::make_unique<SherpaOnnxAsrEngine>();
        break;
    case AsrEngineKind::SherpaOffline:
        engine = std::make_unique<SherpaOfflineAsrEngine>();
        break;
    case AsrEngineKind::OpenaiRealtime:
        engine = std::make_unique<RealtimeAsrEngine>();
        break;
    case AsrEngineKind::Openai:
    default:
        engine = std::make_unique<OpenaiAsrEngine>();
        break;
    }
    if (!engine) return nullptr;

    try {
        if (!engine->Init(snapshot.config)) return nullptr;
    } catch (const std::exception& e) {
        // 只记录异常类型，不输出可能含凭据/路径的 what() 之外的上下文。
        FCITX_ERROR() << "[voice-input] ASR init threw: backend="
                      << snapshot.backend;
        return nullptr;
    } catch (...) {
        FCITX_ERROR() << "[voice-input] ASR init threw: backend="
                      << snapshot.backend;
        return nullptr;
    }
    return engine;
}

// -------------------------------------------------------------
// VoiceInputEngine 的后端构造与切换
// -------------------------------------------------------------
AsrBackendSnapshot VoiceInputEngine::SnapshotActiveAsrBackend() const {
    AsrBackendSnapshot snapshot;
    snapshot.backend = *config_.activeBackend;
    auto& cfg = snapshot.config;

    if (snapshot.backend == "volcengine") {
        snapshot.kind = AsrEngineKind::Volcengine;
        cfg.apiEndpoint = *volcengineConfig_.endpoint;
        if (EndpointUsesPlaintext(cfg.apiEndpoint)) {
            FCITX_WARN() << "[voice-input] Volcengine endpoint is not TLS, "
                         << "API credentials will be sent in plaintext: "
                         << cfg.apiEndpoint;
        }
        cfg.apiKey = *volcengineConfig_.apiKey;
        cfg.authMode = *volcengineConfig_.authMode;
        cfg.appKey = *volcengineConfig_.appKey;
        cfg.accessKey = *volcengineConfig_.accessKey;
        cfg.resourceId = *volcengineConfig_.resourceId;
        cfg.modelName = "bigmodel";
        cfg.chunkMs = *volcengineConfig_.chunkMs;
        cfg.enableItN = *volcengineConfig_.enableITN;
        cfg.enablePunc = *volcengineConfig_.enablePunc;
        cfg.enableDdc = *volcengineConfig_.enableDDC;
        cfg.enableNonstream = *volcengineConfig_.enableNonstream;
        cfg.endWindowMs = *volcengineConfig_.endWindowMs;
        FCITX_INFO() << "[voice-input] Volcengine config: endpoint="
                     << cfg.apiEndpoint << " apiKey="
                     << (cfg.apiKey.empty() ? "(empty)" : "***");
    } else if (snapshot.backend == "mistral") {
        snapshot.kind = AsrEngineKind::Mistral;
        cfg.apiEndpoint = *mistralConfig_.baseUrl;
        if (EndpointUsesPlaintext(cfg.apiEndpoint)) {
            FCITX_WARN() << "[voice-input] Mistral endpoint is not TLS, "
                         << "API credentials will be sent in plaintext: "
                         << cfg.apiEndpoint;
        }
        cfg.apiKey = *mistralConfig_.apiKey;
        cfg.modelName = *mistralConfig_.model;
        cfg.commitIntervalMs = *mistralConfig_.commitIntervalMs;
        cfg.targetStreamingDelayMs = *mistralConfig_.targetStreamingDelayMs;
        FCITX_INFO() << "[voice-input] Mistral config: endpoint="
                     << cfg.apiEndpoint << " model=" << cfg.modelName
                     << " delay=" << cfg.targetStreamingDelayMs;
    } else if (snapshot.backend == "sherpa_onnx") {
        snapshot.kind = AsrEngineKind::SherpaOnnx;
        snapshot.local = true;
        cfg.numThreads = *sherpaOnnxConfig_.numThreads;
        cfg.hotwordsFile = *sherpaOnnxConfig_.hotwordsFile;
        cfg.hotwordsScore =
            static_cast<float>(*sherpaOnnxConfig_.hotwordsScore);
        FCITX_INFO() << "[voice-input] SherpaOnnx config: threads="
                     << cfg.numThreads << " hotwordsScore=" << cfg.hotwordsScore;
    } else if (snapshot.backend == "sherpa_offline") {
        snapshot.kind = AsrEngineKind::SherpaOffline;
        snapshot.local = true;
        cfg.numThreads = *sherpaOfflineConfig_.numThreads;
        cfg.hotwordsFile = *sherpaOfflineConfig_.hotwordsFile;
        cfg.hotwordsScore =
            static_cast<float>(*sherpaOfflineConfig_.hotwordsScore);
        FCITX_INFO() << "[voice-input] SherpaOffline config: threads="
                     << cfg.numThreads << " hotwordsScore=" << cfg.hotwordsScore;
    } else {
        cfg.apiEndpoint = *openaiConfig_.baseUrl;
        if (EndpointUsesPlaintext(cfg.apiEndpoint)) {
            FCITX_WARN() << "[voice-input] OpenAI endpoint is not TLS, "
                         << "API credentials will be sent in plaintext: "
                         << cfg.apiEndpoint;
        }
        cfg.apiKey = *openaiConfig_.apiKey;
        cfg.modelName = *openaiConfig_.model;
        cfg.apiMode = *openaiConfig_.apiMode;
        cfg.enableItn = *openaiConfig_.enableItn;
        cfg.commitIntervalMs = *openaiConfig_.commitIntervalMs;
        auto language = *openaiConfig_.language;
        if (language == "auto") language.clear();
        cfg.language = language;
        snapshot.kind = (cfg.apiMode == "realtime")
                            ? AsrEngineKind::OpenaiRealtime
                            : AsrEngineKind::Openai;
        FCITX_INFO() << "[voice-input] OpenAI config: endpoint="
                     << cfg.apiEndpoint << " model=" << cfg.modelName
                     << " apiMode=" << cfg.apiMode
                     << " enableItn=" << cfg.enableItn;
    }
    return snapshot;
}

void VoiceInputEngine::ReloadAsrBackend(const char* reason) {
    if (!initialized_) return;

    const uint64_t generation = ++asrPrepareGeneration_;
    const AsrBackendSnapshot snapshot = SnapshotActiveAsrBackend();
    // 离开某本地后端后，它的状态镜像必须回到 idle，否则 GUI 会误报 ready/loading。
    ResetInactiveLocalStatuses(snapshot.backend);

    if (!snapshot.local) {
        // 云端 Init 不触网（各引擎只保存配置），保持同步装载以获得零延迟。
        // 在途的本地准备必须作废：否则用户切到云端后旧结果仍会被应用。
        asrPreparer_->Cancel();
        auto engine = CreateAsrEngine(snapshot);
        if (engine) {
            pipeline_->SetAsrEngine(std::move(engine));
            installedBackend_ = snapshot.backend;
            UpdateModelStatus(snapshot.backend, "ready", "");
            FCITX_INFO() << "[voice-input] ASR backend ready: backend="
                         << snapshot.backend << " reason=" << reason;
        } else {
            if (installedBackend_ != snapshot.backend) DropAsrEngine();
            UpdateModelStatus(snapshot.backend, "failed", "init_failed");
            FCITX_WARN() << "[voice-input] ASR backend failed: backend="
                         << snapshot.backend << " reason=" << reason;
        }
        return;
    }

    // 本地模型：Init 会加载数百 MB 的 ONNX 模型，必须在后台完成。
    const bool keepOld = (installedBackend_ == snapshot.backend);
    if (!keepOld) {
        // 后端切换（含云端 → 本地）：立刻摘除旧引擎并作废旧结果，
        // 防止旧云端继续接收音频；准备失败也不回退到旧后端。
        DropAsrEngine();
    }
    UpdateModelStatus(snapshot.backend, "loading",
                      keepOld ? "reloading" : "loading");
    if (!keepOld) {
        // 旧引擎已被摘除：明确告知用户正在准备模型，而不是显示“就绪”。
        SetStatus(_("本地模型加载中..."));
    }
    FCITX_INFO() << "[voice-input] ASR prepare requested: backend="
                 << snapshot.backend << " gen=" << generation
                 << " keepOld=" << keepOld << " reason=" << reason;
    asrPreparer_->Request(generation, snapshot);
}

void VoiceInputEngine::ApplyPreparedAsrBackend(
    uint64_t generation, std::unique_ptr<AsrEngine> engine,
    const std::string& error) {
    // 代次过滤：只有最新一次请求的完成结果才能生效。
    if (generation != asrPrepareGeneration_) {
        FCITX_INFO() << "[voice-input] ASR prepare superseded: gen="
                     << generation << " current=" << asrPrepareGeneration_;
        return;
    }
    const std::string backend = *config_.activeBackend;
    if (!IsLocalAsrBackend(backend)) {
        FCITX_INFO() << "[voice-input] ASR prepare dropped: backend changed to "
                     << backend;
        return;
    }

    if (!engine) {
        FCITX_WARN() << "[voice-input] local model unavailable: backend="
                     << backend << " reason=" << error;
        if (installedBackend_ != backend) {
            // 没有可用引擎：明确失败，也不回退云端。
            DropAsrEngine();
            UpdateModelStatus(backend, "failed", error);
            SetStatus(_("语音识别失败"));
        } else {
            // 同一后端重载失败：保留旧引擎继续工作，状态明确标记失败。
            UpdateModelStatus(backend, "failed", error);
            SetStatus(_("模型加载失败，继续使用原模型"));
        }
        return;
    }

    // 身份必须在移交引擎前读取（移交后由 pipeline 持有）。
    const AsrModelIdentity identity = engine->ModelIdentity();
    pipeline_->SetAsrEngine(std::move(engine));
    installedBackend_ = backend;
    UpdateModelStatus(backend, "ready", "");
    UpdateModelIdentity(backend, identity);
    FCITX_INFO() << "[voice-input] local model ready: backend=" << backend
                 << " gen=" << generation
                 << " model=" << (identity.valid ? identity.id : "(unknown)")
                 << " version=" << (identity.valid ? identity.version : "(unknown)");

    // 异步完成时若仍在语音输入上下文中，启动 pipeline（此前 Start 因无引擎提前返回）。
    if (activeIc_.get() && sessionGeneration_.load() != 0 &&
        !pipeline_->IsRunning()) {
        pipeline_->Start();
    }
    SetStatus(pipeline_->IsRunning() ? _("语音输入就绪") : _("语音识别失败"));
}

void VoiceInputEngine::DropAsrEngine() {
    // SetAsrEngine(nullptr)：SkipAllSessions + 取消旧引擎全部会话，
    // 之后 AsrDispatcherLoop 不会再用旧引擎建立会话（含云端）。
    const std::string dropped = installedBackend_;
    pipeline_->SetAsrEngine(std::unique_ptr<AsrEngine>());
    installedBackend_.clear();
    if (!dropped.empty()) {
        // 没有引擎在服务：状态镜像不能继续显示 ready/loading 与旧版本。
        UpdateModelStatus(dropped, "idle", "");
        UpdateModelIdentity(dropped, AsrModelIdentity{});
    }
    RotateAsrGeneration();
}

void VoiceInputEngine::RotateAsrGeneration() {
    if (!activeIc_.get() || sessionGeneration_.load() == 0) return;
    const uint64_t generation = activeGeneration_.fetch_add(1) + 1;
    sessionGeneration_.store(generation);
    pipeline_->SetGeneration(generation);
    FCITX_INFO() << "[voice-input] ASR generation rotated: gen=" << generation;
}

void VoiceInputEngine::UpdateModelStatus(const std::string& backend,
                                         const char* state,
                                         const std::string& detail) {
    ModelRuntimeStatusConfig* status = nullptr;
    if (backend == "sherpa_onnx") {
        status = &onlineModelStatus_;
    } else if (backend == "sherpa_offline") {
        status = &offlineModelStatus_;
    }
    if (!status) return;
    status->state.setValue(state);
    status->detail.setValue(detail);
}

void VoiceInputEngine::UpdateModelIdentity(const std::string& backend,
                                           const AsrModelIdentity& identity) {
    ModelRuntimeStatusConfig* status = nullptr;
    if (backend == "sherpa_onnx") {
        status = &onlineModelStatus_;
    } else if (backend == "sherpa_offline") {
        status = &offlineModelStatus_;
    }
    if (!status) return;
    // 后端没有报告身份时保持为空，不编造版本。
    status->model.setValue(identity.valid ? identity.id : std::string());
    status->version.setValue(identity.valid ? identity.version : std::string());
}

void VoiceInputEngine::ResetInactiveLocalStatuses(
    const std::string& activeBackend) {
    // 未被选中的本地后端不应继续展示上一次的 ready/loading/failed。
    if (activeBackend != "sherpa_onnx") {
        UpdateModelStatus("sherpa_onnx", "idle", "");
        UpdateModelIdentity("sherpa_onnx", AsrModelIdentity{});
    }
    if (activeBackend != "sherpa_offline") {
        UpdateModelStatus("sherpa_offline", "idle", "");
        UpdateModelIdentity("sherpa_offline", AsrModelIdentity{});
    }
}

} // namespace fcitx
