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
#include <fcitx/instance.h>
#include <fcitx/userinterface.h>

#include "engine.h"

#include "asr/openai_asr.h"

#ifdef ENABLE_SHERPA_ONNX
#include "asr/sherpa_asr.h"
#endif

namespace fcitx {

VoiceInputEngine::VoiceInputEngine(Instance *instance)
    : instance_(instance), pipeline_(std::make_unique<Pipeline>()) {
    eventDispatcher_.attach(&instance_->eventLoop());
    reloadConfig();
}

VoiceInputEngine::~VoiceInputEngine() { pipeline_->Abort(); }

void VoiceInputEngine::reloadConfig() {
    readAsIni(config_, "conf/voiceinput.conf");
}

void VoiceInputEngine::setConfig(const RawConfig &rawConfig) {
    config_.load(rawConfig, true);

    bool saved = safeSaveAsIni(config_, "conf/voiceinput.conf");
    FCITX_INFO() << "[voice-input] setConfig saved=" << saved;

    // Re-apply config to pipeline if initialized
    if (initialized_) {
        pipeline_->SetConfig(config_);
    }
}

void VoiceInputEngine::activate(const InputMethodEntry &entry,
                                InputContextEvent &event) {
    FCITX_UNUSED(entry);
    FCITX_UNUSED(event);
    InitializeIfNeeded();
    activeIc_ = event.inputContext();
    uint64_t generation = activeGeneration_.fetch_add(1) + 1;
    pendingStopGeneration_ = generation;
    sessionGeneration_.store(generation);
    if (pipeline_->GetState() == Pipeline::State::IDLE) {
        pipeline_->StartListening();
    }
    statusText_.clear();
    if (activeIc_) {
        activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
    }
}

void VoiceInputEngine::deactivate(const InputMethodEntry &entry,
                                  InputContextEvent &event) {
    FCITX_UNUSED(entry);
    FCITX_UNUSED(event);
    uint64_t generation = activeGeneration_.fetch_add(1) + 1;
    pendingStopGeneration_ = generation;
    ClearUI();

    delayedStopEvent_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC,
        now(CLOCK_MONOTONIC) + 200000,
        0,
        [this, generation](EventSourceTime *, uint64_t) {
            if (pendingStopGeneration_ != generation) {
                return true;
            }
            sessionGeneration_.store(0);
            if (pipeline_->GetState() != Pipeline::State::IDLE) {
                pipeline_->StopListening();
            }
            activeIc_ = nullptr;
            return true;
        });
    delayedStopEvent_->setOneShot();
}

std::vector<InputMethodEntry> VoiceInputEngine::listInputMethods() {
    std::vector<InputMethodEntry> entries;
    entries.emplace_back("voiceinput", _("Voice Input"), "zh_CN",
                         "voiceinput");
    entries.back().setLabel("🎙").setConfigurable(true);
    return entries;
}

void VoiceInputEngine::keyEvent(const InputMethodEntry &entry,
                                KeyEvent &keyEvent) {
    FCITX_UNUSED(entry);
    FCITX_UNUSED(keyEvent);
}

void VoiceInputEngine::OnPipelineStateChange(Pipeline::State oldState,
                                              Pipeline::State newState) {
    FCITX_UNUSED(oldState);
    FCITX_DEBUG() << "[voice-input] State change: " << pipeline_->StateName();

    uint64_t generation = sessionGeneration_.load();

    // State transitions from pipeline/capture threads → dispatch to main loop
    if (newState == Pipeline::State::LISTENING) {
        eventDispatcher_.schedule([this, generation]() {
            if (generation != 0 && activeGeneration_.load() == generation && activeIc_) {
                statusText_.clear();
                activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
            }
        });
    } else if (newState == Pipeline::State::RECORDING) {
        eventDispatcher_.schedule([this, generation]() {
            if (generation != 0 && activeGeneration_.load() == generation && activeIc_) {
                statusText_ = "🎙 录音中...";
                activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
            }
        });
    } else if (newState == Pipeline::State::PROCESSING_ASR) {
        eventDispatcher_.schedule([this, generation]() {
            if (generation != 0 && activeGeneration_.load() == generation && activeIc_) {
                statusText_ = "⏳ 转录中...";
                activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
            }
        });
    } else if (newState == Pipeline::State::IDLE) {
        eventDispatcher_.schedule([this, generation]() {
            if (generation != 0 && activeGeneration_.load() == generation && activeIc_) {
                statusText_.clear();
                activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
            }
        });
    }
}

void VoiceInputEngine::OnAsrResult(const std::string &text) {
    uint64_t generation = sessionGeneration_.load();

    eventDispatcher_.schedule([this, generation, text]() {
        if (generation != 0 && activeGeneration_.load() == generation && activeIc_) {
            if (!text.empty())
                activeIc_->commitString(text);
        }
    });
}

void VoiceInputEngine::CommitText(const std::string &text) {
    auto *ic = activeIc_;
    if (ic) {
        ic->commitString(text);
    }
}

void VoiceInputEngine::SetUIStatus(const std::string &text, bool instant) {
    FCITX_UNUSED(instant);
    FCITX_INFO() << "[voice-input] SetUIStatus text='" << text << "'";

    eventDispatcher_.schedule([this, text]() {
        statusText_ = text;
        if (activeIc_) {
            activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
        }
    });
}

void VoiceInputEngine::ClearUI() {
    FCITX_INFO() << "[voice-input] ClearUI ic_nonnull=" << (activeIc_ != nullptr);

    eventDispatcher_.schedule([this]() {
        statusText_.clear();
        if (activeIc_) {
            activeIc_->inputPanel().reset();
            activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
            activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
        }
    });
}

std::string VoiceInputEngine::subModeLabelImpl(const InputMethodEntry &entry,
                                                InputContext &ic) {
    FCITX_UNUSED(entry);
    if (&ic == activeIc_ && !statusText_.empty()) {
        return statusText_;
    }
    return {};
}

void VoiceInputEngine::InitializeIfNeeded() {
    if (initialized_)
        return;
    initialized_ = true;

    // Setup pipeline callbacks
    pipeline_->SetStateCallback(
        [this](Pipeline::State oldState, Pipeline::State newState) {
            OnPipelineStateChange(oldState, newState);
        });

    pipeline_->SetResultCallback(
        [this](const std::string &text) { OnAsrResult(text); });

    pipeline_->Init(config_);

    // Create ASR engine based on backend selection
    auto asrConfig = AsrEngine::Config{};

    if (config_.asrBackend.value() == "sherpa-onnx") {
#ifdef ENABLE_SHERPA_ONNX
        asrConfig.modelPath = config_.modelPath.value();
        asrConfig.modelName = config_.modelName.value();
        asrConfig.numThreads = config_.numThreads.value();

        auto asr = std::make_unique<SherpaAsrEngine>();
        if (asr->Init(asrConfig)) {
            pipeline_->SetAsrEngine(std::move(asr));
        } else {
            FCITX_WARN() << "[voice-input] Sherpa-onnx init failed, "
                            "falling back to OpenAI-compatible";
        }
#else
        FCITX_WARN() << "[voice-input] asrBackend=sherpa-onnx but "
                        "ENABLE_SHERPA_ONNX not set, using OpenAI";
#endif
    }

    // Default: OpenAI-compatible (also serves as fallback)
    if (!pipeline_->HasAsrEngine()) {
        asrConfig.apiEndpoint = config_.openaiEndpoint.value();
        asrConfig.apiKey = config_.openaiApiKey.value();
        asrConfig.modelName = config_.openaiModel.value();
        asrConfig.language = config_.openaiLanguage.value();

        auto asr = std::make_unique<OpenaiCompatAsrEngine>();
        if (asr->Init(asrConfig)) {
            pipeline_->SetAsrEngine(std::move(asr));
            FCITX_INFO() << "[voice-input] Using OpenAI-compatible ASR: "
                         << config_.openaiEndpoint.value()
                         << " model=" << config_.openaiModel.value();
        } else {
            FCITX_WARN() << "[voice-input] OpenAI ASR init failed "
                            "(no API key?), running capture-only";
        }
    }
}

} // namespace fcitx

// Fcitx5 addon factory — must be outside fcitx namespace for
// FCITX_ADDON_FACTORY_V2 (which expands to extern "C").
class VoiceInputAddonFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new fcitx::VoiceInputEngine(manager->instance());
    }
};
FCITX_ADDON_FACTORY(VoiceInputAddonFactory);
