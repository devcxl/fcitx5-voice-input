#include <string>

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/log.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <fcitx/instance.h>
#include <fcitx/userinterface.h>

#include "engine.h"

#include "asr/openai_asr.h"
#include "asr/volcengine_asr.h"
#include "llm/llm_client.h"

namespace fcitx {

VoiceInputEngine::VoiceInputEngine(Instance* instance)
    : instance_(instance), pipeline_(std::make_unique<Pipeline>()) {
    fcitx::registerDomain(FCITX_GETTEXT_DOMAIN, VOICE_INPUT_LOCALE_DIR);
    eventDispatcher_.attach(&instance_->eventLoop());
    reloadConfig();
}

VoiceInputEngine::~VoiceInputEngine() {
    pipeline_->Abort();
    eventDispatcher_.detach();
}

void VoiceInputEngine::reloadConfig() {
    readAsIni(config_, "conf/voiceinput.conf");
    readAsIni(openaiConfig_, "conf/voiceinput-openai.conf");
    readAsIni(volcengineConfig_, "conf/voiceinput-volcengine.conf");
    FCITX_INFO() << "[voice-input] reloadConfig: backend="
                 << *config_.activeBackend;
}

void VoiceInputEngine::setConfig(const RawConfig& rawConfig) {
    config_.load(rawConfig, true);
    FCITX_INFO() << "[voice-input] setConfig: backend="
                 << *config_.activeBackend;

    bool saved = safeSaveAsIni(config_, "conf/voiceinput.conf");
    FCITX_INFO() << "[voice-input] setConfig saved=" << saved;

    if (initialized_) {
        pipeline_->SetConfig(config_);
        ReloadActiveAsrClient();
    }
}

const Configuration* VoiceInputEngine::getSubConfig(
    const std::string& path) const {
    FCITX_INFO() << "[voice-input] getSubConfig: path=" << path;
    if (path == "asr/openai") {
        return &openaiConfig_;
    }
    if (path == "asr/volcengine") {
        return &volcengineConfig_;
    }
    FCITX_WARN() << "[voice-input] getSubConfig: unknown path=" << path;
    return nullptr;
}

void VoiceInputEngine::setSubConfig(const std::string& path,
                                    const RawConfig& rawConfig) {
    FCITX_INFO() << "[voice-input] setSubConfig: path=" << path;
    if (path == "asr/openai") {
        openaiConfig_.load(rawConfig, true);
        safeSaveAsIni(openaiConfig_, "conf/voiceinput-openai.conf");
        FCITX_INFO() << "[voice-input] Saved openai sub-config";
    } else if (path == "asr/volcengine") {
        volcengineConfig_.load(rawConfig, true);
        safeSaveAsIni(volcengineConfig_, "conf/voiceinput-volcengine.conf");
        FCITX_INFO() << "[voice-input] Saved volcengine sub-config";
    }

    if (initialized_) {
        ReloadActiveAsrClient();
    }
}

void VoiceInputEngine::activate(const InputMethodEntry& entry,
                                InputContextEvent& event) {
    FCITX_UNUSED(entry);
    FCITX_UNUSED(event);
    InitializeIfNeeded();
    activeIc_ = event.inputContext();
    uint64_t generation = activeGeneration_.fetch_add(1) + 1;
    pendingStopGeneration_ = generation;
    sessionGeneration_.store(generation);

    pipeline_->SetGeneration(generation);

    bool wasRunning = pipeline_->IsRunning();
    pipeline_->Start();

    FCITX_INFO() << "[voice-input] Activate: gen=" << generation
                 << " wasRunning=" << wasRunning
                 << " ic=" << (activeIc_ != nullptr);

    statusText_.clear();
    SetStatus(_("语音输入就绪"));
}

void VoiceInputEngine::deactivate(const InputMethodEntry& entry,
                                  InputContextEvent& event) {
    FCITX_UNUSED(entry);
    FCITX_UNUSED(event);
    uint64_t generation = activeGeneration_.fetch_add(1) + 1;
    pendingStopGeneration_ = generation;

    FCITX_INFO() << "[voice-input] Deactivate: gen=" << generation;

    ClearUI();

    delayedStopEvent_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC,
        now(CLOCK_MONOTONIC) + 200000,
        0,
        [this, generation](EventSourceTime*, uint64_t) {
            if (pendingStopGeneration_ != generation) {
                FCITX_INFO() << "[voice-input] DelayedStop: cancelled gen="
                             << generation;
                return true;
            }
            FCITX_INFO() << "[voice-input] DelayedStop: executing gen="
                         << generation;
            sessionGeneration_.store(0);
            pipeline_->Stop();
            activeIc_ = nullptr;
            return true;
        });
    delayedStopEvent_->setOneShot();
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
    if (!pendingPreeditText_.empty() && activeIc_) {
        activeIc_->commitString(pendingPreeditText_);
        activeIc_->inputPanel().reset();
        activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
        SetStatus(_("语音输入就绪"));
        pendingPreeditText_.clear();
        pendingPreeditUtteranceId_ = 0;
        FCITX_DEBUG() << "[voice-input] Preedit committed on keyEvent";
    }
    FCITX_UNUSED(keyEvent);
}

void VoiceInputEngine::OnAsrResult(const std::string& text) {
    uint64_t generation = sessionGeneration_.load();
    FCITX_INFO() << "[voice-input] OnAsrResult: text='"
                 << text.substr(0, 30) << "'"
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

void VoiceInputEngine::PollResults() {
    auto& queue = pipeline_->ResultQueue();
    AsrResult result;
    while (queue.TryPop(result)) {
        bool valid = !result.text.empty()
                  && result.generation != 0
                  && activeGeneration_.load() == result.generation
                  && activeIc_ != nullptr;

        FCITX_DEBUG() << "[voice-input] PollResult:"
                     << " text=\"" << result.text << "\""
                     << " gen=" << result.generation
                     << " activeGen=" << activeGeneration_.load()
                     << " uid=" << result.utteranceId
                     << " pendingUid=" << pendingPreeditUtteranceId_
                     << " refined=" << result.isLLMRefined
                     << " valid=" << valid;

        if (valid) {
            if (result.isLLMRefined) {
                if (result.isPartial) {
                    // Streaming partial: update preedit in-place
                    if (result.utteranceId == pendingPreeditUtteranceId_) {
                        activeIc_->inputPanel().setPreedit(Text(result.text));
                        activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
                        statusText_ = result.text;
                        activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
                    }
                } else if (result.utteranceId == pendingPreeditUtteranceId_) {
                    FCITX_INFO() << "[voice-input] LLM commit: uid=" << result.utteranceId
                                 << " text=\"" << result.text << "\"";
                    activeIc_->commitString(result.text);
                    activeIc_->inputPanel().reset();
                    activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
                    SetStatus(_("语音输入就绪"));
                    pendingPreeditText_.clear();
                    pendingPreeditUtteranceId_ = 0;
                } else {
                    FCITX_DEBUG() << "[voice-input] LLM stale skip: uid="
                                  << result.utteranceId
                                  << " pendingUid=" << pendingPreeditUtteranceId_;
                }
            } else {
                bool llmActive = openaiConfig_.llmEnabled.value()
                              && !openaiConfig_.llmModel.value().empty();
                FCITX_INFO() << "[voice-input] Preedit: uid=" << result.utteranceId
                             << " text=\"" << result.text << "\""
                             << " llmActive=" << llmActive;

                if (result.isPartial) {
                    activeIc_->inputPanel().setPreedit(Text(result.text));
                    activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
                    statusText_ = result.text;
                    activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
                    continue;
                }

                if (llmActive) {
                    activeIc_->inputPanel().setPreedit(Text(result.text));
                    activeIc_->inputPanel().setAuxDown(Text(_("修正中...")));
                    statusText_ = result.text;
                    activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
                    activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
                    pendingPreeditText_ = result.text;
                    pendingPreeditUtteranceId_ = result.utteranceId;
                } else if (openaiConfig_.autoCommit.value()) {
                    activeIc_->commitString(result.text);
                    activeIc_->inputPanel().reset();
                    activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
                    SetStatus(_("语音输入就绪"));
                    pendingPreeditText_.clear();
                    pendingPreeditUtteranceId_ = 0;
                } else {
                    activeIc_->inputPanel().setPreedit(Text(result.text));
                    statusText_ = result.text;
                    activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
                    activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
                    pendingPreeditText_ = result.text;
                    pendingPreeditUtteranceId_ = result.utteranceId;
                }
            }
        }
    }
}

void VoiceInputEngine::SetStatus(const std::string& text) {
    eventDispatcher_.schedule([this, text]() {
        statusText_ = text;
        if (activeIc_) {
            activeIc_->inputPanel().setAuxDown(Text(text));
            activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
            activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
        }
    });
}

void VoiceInputEngine::ClearUI() {
    uint64_t gen = activeGeneration_.load();
    eventDispatcher_.schedule([this, gen]() {
        if (activeGeneration_.load() != gen) return;
        statusText_.clear();
        if (activeIc_) {
            // Commit pending preedit before clearing
            if (!pendingPreeditText_.empty()) {
                activeIc_->commitString(pendingPreeditText_);
                pendingPreeditText_.clear();
                pendingPreeditUtteranceId_ = 0;
            }
            activeIc_->inputPanel().reset();
            activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
            activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
        }
    });
}

std::string VoiceInputEngine::subModeLabelImpl(const InputMethodEntry& entry,
                                                InputContext& ic) {
    FCITX_UNUSED(entry);
    if (&ic == activeIc_ && !statusText_.empty()) {
        return statusText_;
    }
    return {};
}

std::unique_ptr<AsrEngine> VoiceInputEngine::CreateAsrEngine() {
    auto backend = *config_.activeBackend;
    FCITX_INFO() << "[voice-input] CreateAsrEngine: backend=" << backend;
    auto asrConfig = AsrEngine::Config{};
    std::unique_ptr<AsrEngine> asr;

    if (backend == "volcengine") {
        asrConfig.apiEndpoint = *volcengineConfig_.endpoint;
        asrConfig.apiKey = *volcengineConfig_.apiKey;
        asrConfig.authMode = *volcengineConfig_.authMode;
        asrConfig.appKey = *volcengineConfig_.appKey;
        asrConfig.accessKey = *volcengineConfig_.accessKey;
        asrConfig.resourceId = *volcengineConfig_.resourceId;
        asrConfig.modelName = "bigmodel";
        asrConfig.chunkMs = *volcengineConfig_.chunkMs;
        asrConfig.enableItN = *volcengineConfig_.enableITN;
        asrConfig.enablePunc = *volcengineConfig_.enablePunc;
        asrConfig.enableDdc = *volcengineConfig_.enableDDC;
        asrConfig.enableNonstream = *volcengineConfig_.enableNonstream;
        asrConfig.endWindowMs = *volcengineConfig_.endWindowMs;
        FCITX_INFO() << "[voice-input] Volcengine config: endpoint="
                     << asrConfig.apiEndpoint
                     << " apiKey=" << (asrConfig.apiKey.empty() ? "(empty)" : "***");
        asr = std::make_unique<VolcengineAsrEngine>();
    } else {
        asrConfig.apiEndpoint = *openaiConfig_.baseUrl;
        asrConfig.apiKey = *openaiConfig_.apiKey;
        asrConfig.modelName = *openaiConfig_.model;
        auto language = *openaiConfig_.language;
        if (language == "auto") {
            language.clear();
        }
        asrConfig.language = language;
        FCITX_INFO() << "[voice-input] OpenAI config: endpoint="
                     << asrConfig.apiEndpoint
                     << " model=" << asrConfig.modelName;
        asr = std::make_unique<OpenaiAsrEngine>();
    }

    if (asr->Init(asrConfig)) {
        FCITX_INFO() << "[voice-input] ASR init OK: " << asr->Name();
        return asr;
    }

    FCITX_WARN() << "[voice-input] ASR init failed: " << backend;
    return nullptr;
}

void VoiceInputEngine::ReloadActiveAsrClient() {
    auto asr = CreateAsrEngine();
    if (asr) {
        pipeline_->SetAsrEngine(std::move(asr));
        FCITX_INFO() << "[voice-input] ASR client replaced";
    } else {
        FCITX_WARN() << "[voice-input] ASR client NOT replaced (init failed)";
    }
}

void VoiceInputEngine::InitializeIfNeeded() {
    if (initialized_) return;
    initialized_ = true;

    pipeline_->SetResultCallback(
        [this](const std::string& text) {
            OnAsrResult(text);
        });

    pipeline_->SetVadStatusCallback(
        [this](bool speaking) {
            if (speaking) {
                SetStatus(_("正在录音中..."));
                eventDispatcher_.schedule([this]() {
                    if (!activeIc_) return;
                    activeIc_->inputPanel().setPreedit(Text(" "));
                    activeIc_->updateUserInterface(
                        UserInterfaceComponent::InputPanel);
                });
            } else {
                SetStatus(_("语音输入就绪"));
            }
        });

    pipeline_->Init(config_);

    ReloadActiveAsrClient();

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
