#pragma once

#include <fcitx-utils/eventdispatcher.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/instance.h>

#include "config/voiceinput-config.h"
#include "pipeline/pipeline.h"

namespace fcitx {

/**
 * VoiceInputEngine — Fcitx5 InputMethodEngine addon.
 *
 * Lifecycle:
 *   1. Constructor: register event handlers
 *   2. activate(): create/config pipeline if not yet initialized
 *   3. keyEvent(): handle trigger key for record on/off
 *   4. deactivate(): stop any active recording
 *
 * Thread safety: all Fcitx5 callbacks run on the main event loop thread.
 * ASR runs on a separate thread; results arrive via eventLoop().addDeferredEvent().
 */
class VoiceInputEngine : public InputMethodEngineV2 {
public:
    VoiceInputEngine(Instance *instance);
    ~VoiceInputEngine() override;

    void activate(const InputMethodEntry &entry,
                  InputContextEvent &event) override;

    void deactivate(const InputMethodEntry &entry,
                    InputContextEvent &event) override;

    void keyEvent(const InputMethodEntry &entry,
                  KeyEvent &keyEvent) override;

    // ── Input method registration ─────────────────────────────────────
    std::vector<InputMethodEntry> listInputMethods() override;

    // ── Fcitx5 config tool support ───────────────────────────────────
    const Configuration *getConfig() const override { return &config_; }
    void setConfig(const RawConfig &rawConfig) override;
    void reloadConfig() override;

private:
    void OnPipelineStateChange(Pipeline::State oldState, Pipeline::State newState);
    void OnAsrResult(const std::string &text);
    void CommitText(const std::string &text);
    void InitializeIfNeeded();

    Instance *instance_;
    std::unique_ptr<Pipeline> pipeline_;
    EventDispatcher eventDispatcher_;
    VoiceInputConfig config_;

    InputContext *activeIc_ = nullptr;
    bool initialized_ = false;
};

} // namespace fcitx
