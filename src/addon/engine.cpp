#include "engine.h"

#include <fcitx-utils/log.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx/addonloader.h>
#include <fcitx/addonmanager.h>
#include <fcitx/instance.h>

namespace fcitx {

class VoiceInputAddonFactory : public AddonFactory {
public:
    AddonInstance* create(AddonManager* manager) override {
        return new VoiceInputEngine(manager->instance());
    }
};
FCITX_ADDON_FACTORY(VoiceInputAddonFactory);

VoiceInputEngine::VoiceInputEngine(Instance* instance)
    : instance_(instance)
    , pipeline_(std::make_unique<Pipeline>())
{
    eventDispatcher_.attach(&instance_->eventLoop());
}

VoiceInputEngine::~VoiceInputEngine() {
    pipeline_->Abort();
}

void VoiceInputEngine::activate(const InputMethodEntry& entry,
                                 InputContextEvent& event) {
    InitializeIfNeeded();
    activeIc_ = event.inputContext();
}

void VoiceInputEngine::deactivate(const InputMethodEntry& entry,
                                   InputContextEvent& event) {
    // Stop any active recording
    if (pipeline_->GetState() == Pipeline::State::RECORDING) {
        pipeline_->StopRecording();
    }
    activeIc_ = nullptr;
}

void VoiceInputEngine::keyEvent(const InputMethodEntry& entry,
                                 KeyEvent& keyEvent) {
    auto* ic = keyEvent.inputContext();
    if (!ic) return;

    Key triggerKey = config_.triggerKey;

    // Trigger key pressed → start recording
    if (keyEvent.key() == triggerKey && !keyEvent.isRelease()) {
        if (pipeline_->GetState() == Pipeline::State::IDLE) {
            pipeline_->StartRecording();
            activeIc_ = ic;
            keyEvent.filter();
            return;
        }
        return;
    }

    // Trigger key released → stop recording
    if (keyEvent.key() == triggerKey && keyEvent.isRelease()) {
        if (pipeline_->GetState() == Pipeline::State::RECORDING) {
            pipeline_->StopRecording();
            keyEvent.filter();
            return;
        }
        return;
    }

    return;
}

void VoiceInputEngine::OnPipelineStateChange(
    Pipeline::State oldState, Pipeline::State newState) {
    FCITX_DEBUG() << "[voice-input] State: "
                  << pipeline_->StateName();
}

void VoiceInputEngine::OnAsrResult(const std::string& text) {
    // This is called from the ASR thread.
    // Dispatch to main thread via EventDispatcher::schedule().
    // The IC pointer is captured by value; when the deferred event fires
    // we verify activeIc_ hasn't been reassigned.

    auto* ic = activeIc_;
    if (!ic) return;

    std::string result = text;

    eventDispatcher_.schedule(
        [this, ic, result]() {
            if (activeIc_ == ic) {
                ic->commitString(result);
            }
        });
}

void VoiceInputEngine::CommitText(const std::string& text) {
    if (activeIc_) {
        activeIc_->commitString(text);
    }
}

void VoiceInputEngine::InitializeIfNeeded() {
    if (initialized_) return;
    initialized_ = true;

    LoadConfig();

    // Setup pipeline callbacks
    pipeline_->SetStateCallback(
        [this](Pipeline::State oldState, Pipeline::State newState) {
            OnPipelineStateChange(oldState, newState);
        });

    pipeline_->SetResultCallback(
        [this](const std::string& text) {
            OnAsrResult(text);
        });

    pipeline_->Init(config_);

    // ⚠️ TODO: Create and set real sherpa-onnx ASR engine
    // This requires sherpa-onnx library to be installed at build time.
    // For now, pipeline runs capture-only as a stub.
    //
    // auto asr = std::make_unique<SherpaAsrEngine>();
    // AsrEngine::Config asrConfig;
    // asrConfig.modelPath = config_.modelPath;
    // asrConfig.modelName = config_.modelName;
    // asrConfig.numThreads = config_.numThreads;
    // if (asr->Init(asrConfig)) {
    //     pipeline_->SetAsrEngine(std::move(asr));
    // }
}

void VoiceInputEngine::LoadConfig() {
    // Load from Fcitx config subsystem
    // For now, use defaults since Fcitx config loading integration
    // requires the addon config registration in .conf file.
    config_ = VoiceInputConfig::Defaults();
}

} // namespace fcitx
