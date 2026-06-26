#include <algorithm>

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/standardpath.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/instance.h>

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

    FCITX_INFO() << "[voice-input] setConfig called, TriggerKeys='"
                 << rawConfig.valueByPath("TriggerKeys") << "'";

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
}

void VoiceInputEngine::deactivate(const InputMethodEntry &entry,
                                  InputContextEvent &event) {
    FCITX_UNUSED(entry);
    FCITX_UNUSED(event);
    if (pipeline_->GetState() == Pipeline::State::RECORDING) {
        pipeline_->StopRecording();
    }
    activeIc_ = nullptr;
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

    const auto &triggerKeys = config_.triggerKeys.value();
    const auto &key = keyEvent.key();

    bool matched = std::ranges::any_of(
        triggerKeys, [&key](const Key &tk) { return key == tk; });

    FCITX_INFO() << "[voice-input] keyEvent sym=" << key.sym()
                 << " isRelease=" << keyEvent.isRelease()
                 << " matched=" << matched
                 << " triggerKeys.size=" << triggerKeys.size();

    if (!matched)
        return;

    keyEvent.filter();

    if (!keyEvent.isRelease()) {
        if (pipeline_->GetState() == Pipeline::State::IDLE) {
            FCITX_INFO() << "[voice-input] StartRecording";
            pipeline_->StartRecording();
        }
    } else {
        if (pipeline_->GetState() == Pipeline::State::RECORDING) {
            FCITX_INFO() << "[voice-input] StopRecording";
            pipeline_->StopRecording();
        }
    }
}

void VoiceInputEngine::OnPipelineStateChange(Pipeline::State oldState,
                                             Pipeline::State newState) {
    FCITX_UNUSED(oldState);
    FCITX_DEBUG() << "[voice-input] State change: " << pipeline_->StateName();
}

void VoiceInputEngine::OnAsrResult(const std::string &text) {
    auto *ic = activeIc_;
    if (!ic)
        return;

    eventDispatcher_.schedule([this, ic, text]() {
        if (activeIc_ == ic) {
            ic->commitString(text);
        }
    });
}

void VoiceInputEngine::CommitText(const std::string &text) {
    auto *ic = activeIc_;
    if (ic) {
        ic->commitString(text);
    }
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
