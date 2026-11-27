#include <chrono>
#include <string>
#include <sys/stat.h>

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/log.h>
// Ubuntu 24.04 等旧发行版的 fcitx5 仅有弃用的 standardpath.h，
// 新版（fcitx5 >= 5.1.x）提供 standardpaths.h，条件编译兼容两者
#if __has_include(<fcitx-utils/standardpaths.h>)
#include <fcitx-utils/standardpaths.h>
#define VOICE_INPUT_HAS_STANDARDPATHS
#else
#include <fcitx-utils/standardpath.h>
#endif
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <fcitx/instance.h>
#include <fcitx/userinterface.h>

#include "engine.h"

#include "llm/llm_client.h"

namespace fcitx {

namespace {

using Clock = std::chrono::steady_clock;

auto Milliseconds(Clock::time_point start, Clock::time_point end = Clock::now()) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

// 配置文件可能含 API Key 等敏感凭据，保存后收紧为仅所有者可读写。
// 必须使用 PkgConfig 类型：readAsIni/safeSaveAsIni 默认写的就是
// StandardPathsType::PkgConfig（$XDG_CONFIG_HOME/fcitx5），用 Config
// 会 chmod 到 $XDG_CONFIG_HOME/conf/... 这个不存在的路径，凭据文件保持 0644。
void RestrictConfigFilePermissions(const std::string& relativePath) {
#ifdef VOICE_INPUT_HAS_STANDARDPATHS
    auto configDir =
        StandardPaths::global().userDirectory(StandardPathsType::PkgConfig);
    std::string fullPath = (configDir / relativePath).string();
#else
    auto configDir =
        StandardPath::global().userDirectory(StandardPath::Type::PkgConfig);
    std::string fullPath = configDir + "/" + relativePath;
#endif
    if (::chmod(fullPath.c_str(), S_IRUSR | S_IWUSR) != 0) {
        FCITX_WARN() << "[voice-input] Failed to set 0600 permissions on "
                     << fullPath;
    }
}

// 端点明文警告与后端工厂见 engine_backend.cpp（需要在后台线程安全执行）。

} // namespace

VoiceInputEngine::VoiceInputEngine(Instance* instance)
    : instance_(instance), pipeline_(std::make_unique<Pipeline>()) {
#ifndef HAVE_MODEL_MANAGER_GUI
    // 不展示无法打开的入口；仅在显式禁用 GUI 或未编译 Sherpa 时隐藏。
    sherpaOnnxConfig_.modelManager.setHidden(true);
    sherpaOfflineConfig_.modelManager.setHidden(true);
#endif
    fcitx::registerDomain(FCITX_GETTEXT_DOMAIN, VOICE_INPUT_LOCALE_DIR);
    eventDispatcher_.attach(&instance_->eventLoop());
    // 完成回调只在主线程、且只在本对象存活期间被调用（定时器随本对象析构）。
    asrPreparer_ = std::make_unique<AsrPrepareCoordinator>(
        instance_->eventLoop(),
        [](const AsrBackendSnapshot& snapshot) {
            return CreateAsrEngine(snapshot);
        },
        [this](uint64_t generation, std::unique_ptr<AsrEngine> engine,
               const std::string& error) {
            ApplyPreparedAsrBackend(generation, std::move(engine), error);
        });
    reloadConfig();
    // Core calls deactivate with the original FocusOut/SwitchInputMethod event
    // (fcitx5 5.1.22 instance.cpp), not its later notification event.
    eventWatchers_.push_back(instance_->watchEvent(
        EventType::InputContextDestroyed, EventWatcherPhase::PreInputMethod,
        [this](Event& event) {
            auto& contextEvent = static_cast<InputContextEvent&>(event);
            if (activeIc_.get() == contextEvent.inputContext()) {
                pendingPreeditText_.clear();
                DeactivateContext(true);
            }
        }));
    eventWatchers_.push_back(instance_->watchEvent(
        EventType::InputContextSwitchInputMethod, EventWatcherPhase::PostInputMethod,
        [this](Event& event) {
            auto& switched = static_cast<InputContextSwitchInputMethodEvent&>(event);
            // Core skips engine callbacks for unfocused contexts. This also
            // catches an explicit tray/remote switch after opening the mixer.
            // Other contexts' non-voice switches do not own this capture stream.
            if (switched.oldInputMethod() == "voiceinput" &&
                captureOwnerIc_.get() == switched.inputContext() &&
                instance_->inputMethod(switched.inputContext()) != "voiceinput") {
                DeactivateContext(false);
            }
        }));
    eventWatchers_.push_back(instance_->watchEvent(
        EventType::InputMethodGroupAboutToChange, EventWatcherPhase::PostInputMethod,
        [this](Event&) {
            if (initialized_) DeactivateContext(false);
        }));
}

VoiceInputEngine::~VoiceInputEngine() {
    eventWatchers_.clear();
    // 不 join 后台准备线程：它只持有自己的快照与结果槽，实例销毁后仍可安全跑完
    // 并自行析构结果（详见 engine_backend.h 的生命周期约束）。
    asrPreparer_.reset();
    pipeline_->Abort();
    eventDispatcher_.detach();
}

void VoiceInputEngine::reloadConfig() {
    readAsIni(config_, "conf/voiceinput.conf");
    readAsIni(openaiConfig_, "conf/voiceinput-openai.conf");
    readAsIni(volcengineConfig_, "conf/voiceinput-volcengine.conf");
    readAsIni(mistralConfig_, "conf/voiceinput-mistral.conf");
    readAsIni(sherpaOnnxConfig_, "conf/voiceinput-sherpa-onnx.conf");
    readAsIni(sherpaOfflineConfig_, "conf/voiceinput-sherpa-offline.conf");
    FCITX_INFO() << "[voice-input] reloadConfig: backend="
                 << *config_.activeBackend;
}

void VoiceInputEngine::setConfig(const RawConfig& rawConfig) {
    const std::string previousBackend = *config_.activeBackend;
    config_.load(rawConfig, true);
    FCITX_INFO() << "[voice-input] setConfig: backend="
                 << *config_.activeBackend;

    bool saved = safeSaveAsIni(config_, "conf/voiceinput.conf");
    RestrictConfigFilePermissions("conf/voiceinput.conf");
    FCITX_INFO() << "[voice-input] setConfig saved=" << saved;

    if (!initialized_) return;
    pipeline_->SetConfig(config_);

    // 只在后端选择变化、或当前没有可用引擎且没有在途准备时重建：
    // 仅调整 VAD 参数不该重新加载本地模型。
    const bool backendChanged = (*config_.activeBackend != previousBackend);
    const bool needsEngine =
        installedBackend_.empty() && !asrPreparer_->IsPreparing();
    if (backendChanged || needsEngine) {
        ReloadAsrBackend(backendChanged ? "backend-changed" : "engine-missing");
    }
}

const Configuration* VoiceInputEngine::getSubConfig(
    const std::string& path) const {
    // GUI 会周期性轮询状态路径，不能刷 INFO。
    FCITX_DEBUG() << "[voice-input] getSubConfig: path=" << path;
    if (path == "asr/openai") {
        return &openaiConfig_;
    }
    if (path == "asr/volcengine") {
        return &volcengineConfig_;
    }
    if (path == "asr/mistral") {
        return &mistralConfig_;
    }
    if (path == "asr/sherpa_onnx") {
        return &sherpaOnnxConfig_;
    }
    if (path == "asr/sherpa_offline") {
        return &sherpaOfflineConfig_;
    }
    // 只读运行状态镜像（不落盘；写入被 setSubConfig 忽略），供 GUI 轮询。
    if (path == "model/status/online") {
        return &onlineModelStatus_;
    }
    if (path == "model/status/offline") {
        return &offlineModelStatus_;
    }
    FCITX_WARN() << "[voice-input] getSubConfig: unknown path=" << path;
    return nullptr;
}

namespace {

// 子配置路径对应的后端名；空串表示该路径不影响任何后端。
std::string BackendForSubConfigPath(const std::string& path) {
    if (path == "asr/openai") return "openai";
    if (path == "asr/volcengine") return "volcengine";
    if (path == "asr/mistral") return "mistral";
    if (path == "asr/sherpa_onnx") return "sherpa_onnx";
    if (path == "asr/sherpa_offline") return "sherpa_offline";
    return {};
}

} // namespace

void VoiceInputEngine::setSubConfig(const std::string& path,
                                    const RawConfig& rawConfig) {
    FCITX_INFO() << "[voice-input] setSubConfig: path=" << path;

    // 模型管理器的“模型已变化”通知：这些路径不是可保存配置，
    // 只按需重载对应后端，绝不写 INI。
    if (path == "model/online" || path == "model/offline") {
        const std::string backend =
            (path == "model/online") ? "sherpa_onnx" : "sherpa_offline";
        if (initialized_ && *config_.activeBackend == backend) {
            ReloadAsrBackend("model-notification");
        } else {
            FCITX_INFO() << "[voice-input] model notification ignored: path="
                         << path << " active=" << *config_.activeBackend;
        }
        return;
    }
    // 只读状态镜像：忽略写入，不产生任何副作用。
    if (path == "model/status/online" || path == "model/status/offline") {
        FCITX_INFO() << "[voice-input] status config is read-only: path=" << path;
        return;
    }

    if (path == "asr/openai") {
        openaiConfig_.load(rawConfig, true);
        safeSaveAsIni(openaiConfig_, "conf/voiceinput-openai.conf");
        RestrictConfigFilePermissions("conf/voiceinput-openai.conf");
        FCITX_INFO() << "[voice-input] Saved openai sub-config";
    } else if (path == "asr/volcengine") {
        volcengineConfig_.load(rawConfig, true);
        safeSaveAsIni(volcengineConfig_, "conf/voiceinput-volcengine.conf");
        RestrictConfigFilePermissions("conf/voiceinput-volcengine.conf");
        FCITX_INFO() << "[voice-input] Saved volcengine sub-config";
    } else if (path == "asr/mistral") {
        mistralConfig_.load(rawConfig, true);
        safeSaveAsIni(mistralConfig_, "conf/voiceinput-mistral.conf");
        RestrictConfigFilePermissions("conf/voiceinput-mistral.conf");
        FCITX_INFO() << "[voice-input] Saved mistral sub-config";
    } else if (path == "asr/sherpa_onnx") {
        sherpaOnnxConfig_.load(rawConfig, true);
        safeSaveAsIni(sherpaOnnxConfig_, "conf/voiceinput-sherpa-onnx.conf");
        RestrictConfigFilePermissions("conf/voiceinput-sherpa-onnx.conf");
        FCITX_INFO() << "[voice-input] Saved sherpa_onnx sub-config";
    } else if (path == "asr/sherpa_offline") {
        sherpaOfflineConfig_.load(rawConfig, true);
        safeSaveAsIni(sherpaOfflineConfig_, "conf/voiceinput-sherpa-offline.conf");
        RestrictConfigFilePermissions("conf/voiceinput-sherpa-offline.conf");
        FCITX_INFO() << "[voice-input] Saved sherpa_offline sub-config";
    } else {
        FCITX_WARN() << "[voice-input] setSubConfig: unknown path=" << path;
        return;
    }

    if (!initialized_) return;
    // 非活跃后端的配置变化不重建引擎（避免为无关改动加载本地模型）。
    if (BackendForSubConfigPath(path) == *config_.activeBackend) {
        ReloadAsrBackend("sub-config");
    } else {
        FCITX_INFO() << "[voice-input] sub-config saved without reload: path="
                     << path << " active=" << *config_.activeBackend;
    }
}

void VoiceInputEngine::activate(const InputMethodEntry& entry,
                                InputContextEvent& event) {
    const auto start = Clock::now();
    FCITX_DEBUG() << "[voice-input:timing] stage=engine.activate event=begin";
    FCITX_UNUSED(entry);
    FCITX_UNUSED(event);
    InitializeIfNeeded();
    const auto initDone = Clock::now();
    activeIc_ = event.inputContext()->watch();
    captureOwnerIc_ = event.inputContext()->watch();
    pendingPreeditText_.clear();
    pendingPreeditUtteranceId_ = 0;
    uint64_t generation = activeGeneration_.fetch_add(1) + 1;
    sessionGeneration_.store(generation);

    pipeline_->SetGeneration(generation);

    bool wasRunning = pipeline_->IsRunning();
    const auto pipelineStart = Clock::now();
    pipeline_->Start();
    const auto pipelineDone = Clock::now();

    FCITX_INFO() << "[voice-input] Activate: gen=" << generation
                 << " wasRunning=" << wasRunning
                 << " ic=" << (activeIc_.get() != nullptr);

    statusText_.clear();
    if (pipeline_->IsRunning()) {
        SetStatus(_("语音输入就绪"));
    } else if (asrPreparer_->IsPreparing()) {
        // 本地模型仍在后台准备：此处不是失败，完成时会自动启动 pipeline。
        SetStatus(_("本地模型加载中..."));
    } else {
        SetStatus(_("语音识别失败"));
    }
    FCITX_DEBUG() << "[voice-input:timing] stage=engine.activate event=end init_ms="
                  << Milliseconds(start, initDone) << " pipeline_ms=" << Milliseconds(pipelineStart, pipelineDone)
                  << " total_ms=" << Milliseconds(start);
}

void VoiceInputEngine::deactivate(const InputMethodEntry& entry,
                                  InputContextEvent& event) {
    FCITX_UNUSED(entry);
    if (captureOwnerIc_.get() != event.inputContext()) return;
    DeactivateContext(event.type() == EventType::InputContextFocusOut);
}

void VoiceInputEngine::DeactivateContext(bool keepCapture) {
    const auto start = Clock::now();
    FCITX_DEBUG() << "[voice-input:timing] stage=engine.deactivate event=begin keep_capture=" << keepCapture;
    uint64_t generation = activeGeneration_.fetch_add(1) + 1;
    sessionGeneration_.store(0);

    FCITX_INFO() << "[voice-input] Deactivate: gen=" << generation
                 << " keepCapture=" << keepCapture;

    ClearUI();
    activeIc_.unwatch();
    const auto uiDone = Clock::now();

    if (keepCapture) {
        pipeline_->Pause();
    } else {
        pipeline_->Stop();
        captureOwnerIc_.unwatch();
    }
    FCITX_DEBUG() << "[voice-input:timing] stage=engine.deactivate event=end keep_capture="
                  << keepCapture << " ui_ms=" << Milliseconds(start, uiDone)
                  << " pipeline_ms=" << Milliseconds(uiDone) << " total_ms=" << Milliseconds(start);
}

std::vector<InputMethodEntry> VoiceInputEngine::listInputMethods() {
    std::vector<InputMethodEntry> entries;
    entries.emplace_back("voiceinput", _("Voice Input"), "zh_CN",
                         "voiceinput");
    entries.back().setConfigurable(true);
    return entries;
}

void VoiceInputEngine::keyEvent(const InputMethodEntry& entry,
                                KeyEvent& keyEvent) {
    FCITX_UNUSED(entry);
    // Commit pending preedit on any key press
    auto* ic = activeIc_.get();
    if (!pendingPreeditText_.empty() && ic && ic == keyEvent.inputContext()) {
        ic->commitString(pendingPreeditText_);
        ic->inputPanel().reset();
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        SetStatus(_("语音输入就绪"));
        pendingPreeditText_.clear();
        pendingPreeditUtteranceId_ = 0;
        FCITX_DEBUG() << "[voice-input] Preedit committed on keyEvent";
    }
    FCITX_UNUSED(keyEvent);
}

void VoiceInputEngine::OnAsrResult(const std::string& text) {
    uint64_t generation = sessionGeneration_.load();
    // 语音转写内容属敏感个人信息，仅记录长度而非内容
    FCITX_DEBUG() << "[voice-input] OnAsrResult: len=" << text.size()
                  << " sessionGen=" << generation
                  << " activeGen=" << activeGeneration_.load();
    eventDispatcher_.schedule([this, generation]() {
        if (generation == 0 || activeGeneration_.load() != generation) {
            FCITX_INFO() << "[voice-input] PollResults skipped: gen="
                         << generation << " active=" << activeGeneration_.load();
            return;
        }
        PollResults();
    });
}

void VoiceInputEngine::SetStatus(const std::string& text) {
    uint64_t generation = activeGeneration_.load();
    auto context = activeIc_;
    eventDispatcher_.schedule([this, text, generation, context]() {
        if (activeGeneration_.load() != generation) return;
        statusText_ = text;
        if (auto* ic = context.get()) {
            ic->updateUserInterface(UserInterfaceComponent::StatusArea);
        }
    });
}

void VoiceInputEngine::ClearUI() {
    // deactivate already runs on the main thread. Finish the old context now:
    // deferring this until dispatch can commit its preedit to a new context.
    statusText_.clear();
    if (auto* ic = activeIc_.get()) {
        if (!pendingPreeditText_.empty()) {
            ic->commitString(pendingPreeditText_);
        }
        ic->inputPanel().reset();
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        ic->updateUserInterface(UserInterfaceComponent::StatusArea);
    }
    pendingPreeditText_.clear();
    pendingPreeditUtteranceId_ = 0;
}

std::string VoiceInputEngine::subModeLabelImpl(const InputMethodEntry& entry,
                                                InputContext& ic) {
    FCITX_UNUSED(entry);
    if (&ic == activeIc_.get() && !statusText_.empty()) {
        return statusText_;
    }
    return {};
}


void VoiceInputEngine::InitializeIfNeeded() {
    if (initialized_) return;
    initialized_ = true;

    pipeline_->SetResultCallback(
        [this](const std::string& text) {
            OnAsrResult(text);
        });

    pipeline_->SetVadStatusCallback(
        [this](bool speaking) { OnVadStatus(speaking); });

    pipeline_->Init(config_);

    ReloadAsrBackend("init");

    // LLM post-processing
    bool llmEnabled = openaiConfig_.llmEnabled.value();
    std::string llmModel = openaiConfig_.llmModel.value();
    pipeline_->SetLLMStream(openaiConfig_.llmStream.value());
    if (llmEnabled && !llmModel.empty()) {
        auto llmConfig = LLMClient::Config{};
        llmConfig.endpoint = *openaiConfig_.baseUrl;
        llmConfig.apiKey = *openaiConfig_.apiKey;
        llmConfig.model = llmModel;
        llmConfig.systemPrompt = *openaiConfig_.llmSystemPrompt;

        auto llm = std::make_unique<LLMClient>(std::move(llmConfig));
        pipeline_->SetLLMClient(std::move(llm));
        FCITX_INFO() << "[voice-input] LLM post-processing enabled: "
                     << " model=" << llmModel;
    }
}

} // namespace fcitx

class VoiceInputAddonFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance* create(fcitx::AddonManager* manager) override {
        return new fcitx::VoiceInputEngine(manager->instance());
    }
};
FCITX_ADDON_FACTORY(VoiceInputAddonFactory);
