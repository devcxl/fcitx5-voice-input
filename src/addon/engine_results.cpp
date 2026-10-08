#include "engine.h"

#include <fcitx-utils/i18n.h>
#include <fcitx-utils/log.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>

namespace fcitx {

void VoiceInputEngine::OnVadStatus(bool speaking) {
    uint64_t generation = sessionGeneration_.load();
    // The worker may post only value data. watch()/get() are not thread-safe
    // and must remain on the Fcitx main thread, after generation validation.
    eventDispatcher_.schedule([this, generation, speaking]() {
        if (generation == 0 || activeGeneration_.load() != generation) return;
        auto* ic = activeIc_.get();
        if (!ic) return;
        SetStatus(speaking ? _("正在录音中...") : _("语音输入就绪"));
        if (speaking) {
            ic->inputPanel().setPreedit(Text(" "));
            ic->inputPanel().setAuxDown(Text(_("正在录音中...")));
            ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        }
    });
}

void VoiceInputEngine::PollResults() {
    auto& queue = pipeline_->ResultQueue();
    AsrResult result;
    while (queue.TryPop(result)) {
        auto* ic = activeIc_.get();
        bool valid = (!result.text.empty() || result.isError)
                  && result.generation != 0
                  && activeGeneration_.load() == result.generation
                  && ic != nullptr;

        FCITX_DEBUG() << "[voice-input] PollResult:"
                     << " len=" << result.text.size()
                     << " gen=" << result.generation
                     << " activeGen=" << activeGeneration_.load()
                     << " uid=" << result.utteranceId
                     << " pendingUid=" << pendingPreeditUtteranceId_
                     << " refined=" << result.isLLMRefined
                     << " error=" << result.isError
                     << " valid=" << valid;

        if (valid) {
            if (result.isError) {
                FCITX_WARN() << "[voice-input] PollResult: ASR error uid="
                             << result.utteranceId;
                ic->inputPanel().reset();
                ic->updateUserInterface(UserInterfaceComponent::InputPanel);
                SetStatus(_("语音识别失败"));
                continue;
            }
            if (result.isLLMRefined) {
                if (result.isPartial) {
                    // Streaming partial: update preedit in-place
                    if (result.utteranceId == pendingPreeditUtteranceId_) {
                        ic->inputPanel().setPreedit(Text(result.text));
                        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
                        statusText_ = result.text;
                        ic->updateUserInterface(UserInterfaceComponent::StatusArea);
                    }
                } else if (result.utteranceId == pendingPreeditUtteranceId_) {
                    FCITX_DEBUG() << "[voice-input] LLM commit: uid="
                                  << result.utteranceId
                                  << " len=" << result.text.size();
                    ic->commitString(result.text);
                    ic->inputPanel().reset();
                    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
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
                FCITX_DEBUG() << "[voice-input] Preedit: uid=" << result.utteranceId
                             << " len=" << result.text.size()
                             << " llmActive=" << llmActive;

                if (result.isPartial) {
                    ic->inputPanel().setPreedit(Text(result.text));
                    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
                    statusText_ = result.text;
                    ic->updateUserInterface(UserInterfaceComponent::StatusArea);
                    continue;
                }

                if (llmActive) {
                    ic->inputPanel().setPreedit(Text(result.text));
                    ic->inputPanel().setAuxDown(Text(_("修正中...")));
                    statusText_ = result.text;
                    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
                    ic->updateUserInterface(UserInterfaceComponent::StatusArea);
                    pendingPreeditText_ = result.text;
                    pendingPreeditUtteranceId_ = result.utteranceId;
                } else if (openaiConfig_.autoCommit.value()) {
                    ic->commitString(result.text);
                    ic->inputPanel().reset();
                    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
                    SetStatus(_("语音输入就绪"));
                    pendingPreeditText_.clear();
                    pendingPreeditUtteranceId_ = 0;
                } else {
                    ic->inputPanel().setPreedit(Text(result.text));
                    statusText_ = result.text;
                    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
                    ic->updateUserInterface(UserInterfaceComponent::StatusArea);
                    pendingPreeditText_ = result.text;
                    pendingPreeditUtteranceId_ = result.utteranceId;
                }
            }
        }
    }
}

} // namespace fcitx
