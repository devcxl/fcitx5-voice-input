#pragma once

#include <cstdio>
#include <cstring>
#include <string>

#include <fcitx-config/configuration.h>
#include <fcitx-config/option.h>
#include <fcitx-utils/i18n.h>

namespace fcitx {

struct AudioSourceAnnotation : public EnumAnnotation {
    void dumpDescription(RawConfig &config) const {
        EnumAnnotation::dumpDescription(config);

        // Default (auto-detect) option
        config.setValueByPath("Enum/0", "");
        config.setValueByPath("EnumI18n/0", "Default (Auto)");

        FILE* fp = popen("pactl list sources short 2>/dev/null", "r");
        if (!fp) return;

        char line[512];
        int idx = 1;
        while (fgets(line, sizeof(line), fp)) {
            // Parse: <index>\t<name>\t<driver>\t<sample_spec>\t<state>
            char* tab = std::strchr(line, '\t');
            if (!tab) continue;
            char* name = tab + 1;
            tab = std::strchr(name, '\t');
            if (tab) *tab = '\0';
            if (!*name) continue;

            // Skip monitor sources
            size_t len = std::strlen(name);
            if (len >= 8 && std::strcmp(name + len - 8, ".monitor") == 0) continue;

            config.setValueByPath("Enum/" + std::to_string(idx), name);
            config.setValueByPath("EnumI18n/" + std::to_string(idx), name);
            idx++;
        }
        pclose(fp);
    }
};

FCITX_CONFIGURATION(VoiceInputConfig,
    // ASR backend selection
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

    // Audio source selection (empty = auto-detect)
    // Shown as a dropdown populated from system audio devices
    Option<std::string, NoConstrain<std::string>,
           DefaultMarshaller<std::string>, AudioSourceAnnotation>
        audioSource{this, "AudioSource", _("Audio Source"), ""};

    // LLM post-processing settings
    Option<bool> llmEnabled{this, "LLMEnabled",
                             _("LLM Post-processing"), false};
    Option<std::string> llmModel{this, "LLMModel",
                                  _("LLM Model"), ""};
    Option<std::string> llmSystemPrompt{this, "LLMSystemPrompt",
                                         _("LLM System Prompt"), ""};
    Option<bool> llmStream{this, "LLMStream",
                            _("LLM Streaming"), true};

    // Output behavior
    Option<bool> autoCommit{this, "AutoCommit",
                             _("Auto-Commit when no LLM"), true};

    // VAD (Voice Activity Detection)
    // Stored as 0-100 percentage internally; divide by 100 for 0.0-1.0
    Option<int, IntConstrain> vadThreshold{this, "VADThreshold",
                                             _("VAD Threshold (%)"), 50,
                                             IntConstrain(0, 100)};
    Option<int, IntConstrain> silenceThresholdMs{
        this, "SilenceThresholdMs", _("Silence Threshold (ms)"), 800,
        IntConstrain(100, 10000)};
);

} // namespace fcitx
