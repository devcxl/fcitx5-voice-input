#pragma once

#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputcontext.h>
#include <fcitx/event.h>
#include <fcitx/instance.h>

#include "config/config.h"
#include "pipeline/pipeline.h"

namespace fcitx {

/**
 * VoiceInputEngine — Fcitx5 InputMethodEngine addon.
 *
 * Lifecycle:
 *   1. Constructor: register event handlers
 *   2. activate(): create/config pipeline if not yet initialized
 *   3. keyEvent(): handle trigger key (Alt_R) for record on/off
 *   4. deactivate(): stop any active recording
 *
 * Thread safety: all Fcitx5 callbacks run on the main event loop thread.
 * ASR runs on a separate thread; results arrive via eventLoop().addDeferredEvent().
 */
class VoiceInputEngine : public InputMethodEngineV2 {
public:
    VoiceInputEngine(Instance* instance);
    ~VoiceInputEngine() override;

    // ── InputMethodEngineV2 interface ─────────────────────────────────
    void activate(const InputMethodEntry& entry,
                  InputContextEvent& event) override;

    void deactivate(const InputMethodEntry& entry,
                    InputContextEvent& event) override;

    void keyEvent(const InputMethodEntry& entry,
                  KeyEvent& keyEvent) override;

private:
    // ── Pipeline callbacks ────────────────────────────────────────────
    void OnPipelineStateChange(Pipeline::State oldState, Pipeline::State newState);
    void OnAsrResult(const std::string& text);

    // ── Helpers ───────────────────────────────────────────────────────
    void CommitText(const std::string& text);
    void InitializeIfNeeded();
    void LoadConfig();

    Instance* instance_;
    std::unique_ptr<Pipeline> pipeline_;
    VoiceInputConfig config_;

    // Track active input context for result delivery
    InputContext* activeIc_ = nullptr;

    bool initialized_ = false;
};

} // namespace fcitx
