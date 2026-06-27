#pragma once

#include <fcitx-config/configuration.h>
#include <fcitx-config/option.h>
#include <fcitx-utils/i18n.h>

namespace fcitx {

FCITX_CONFIGURATION(VoiceInputConfig,
    // ASR backend selection: "openai" or "sherpa"
    Option<std::string> asrBackend{this, "ASRBackend",
                                    _("ASR Backend"),
                                    "openai"};

    // OpenAI-compatible API settings
    Option<std::string> openaiEndpoint{this, "OpenAIEndpoint",
                                        _("OpenAI API Endpoint"),
                                        "https://api.openai.com/v1"};
    Option<std::string> openaiApiKey{this, "OpenAIApiKey",
                                      _("OpenAI API Key"), ""};
    Option<std::string> openaiModel{this, "OpenAIModel",
                                     _("OpenAI Model"),
                                     "whisper-1"};
    Option<std::string> openaiLanguage{this, "OpenAILanguage",
                                        _("Output Language"), ""};

    // LLM post-processing settings
    Option<std::string> llmModel{this, "LLMModel",
                                  _("LLM Model"), ""};
    Option<std::string> llmSystemPrompt{this, "LLMSystemPrompt",
                                         _("LLM System Prompt"), ""};

    // VAD (Voice Activity Detection)
    // Stored as 0-100 percentage internally; divide by 100 for 0.0-1.0
    Option<int, IntConstrain> vadThreshold{this, "VADThreshold",
                                             _("VAD Threshold (%)"), 15,
                                             IntConstrain(0, 100)};
    Option<int, IntConstrain> silenceThresholdMs{
        this, "SilenceThresholdMs", _("Silence Threshold (ms)"), 800,
        IntConstrain(100, 10000)};
);

} // namespace fcitx
