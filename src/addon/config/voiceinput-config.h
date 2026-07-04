#pragma once

#include <string>
#include <utility>

#include <fcitx-config/configuration.h>
#include <fcitx-config/option.h>
#include <fcitx-utils/i18n.h>

namespace fcitx {

struct AsrBackendAnnotation : public EnumAnnotation {
    void dumpDescription(RawConfig &config) const {
        EnumAnnotation::dumpDescription(config);
        config.setValueByPath("Enum/0", "openai");
        config.setValueByPath("EnumI18n/0", _("OpenAI Compatible"));
        config.setValueByPath("SubConfigPath/0",
            "fcitx://config/addon/voiceinput/asr/openai");

        config.setValueByPath("Enum/1", "volcengine");
        config.setValueByPath("EnumI18n/1", _("Volcengine Doubao"));
        config.setValueByPath("SubConfigPath/1",
            "fcitx://config/addon/voiceinput/asr/volcengine");
    }
};

struct VolcengineAuthModeAnnotation : public EnumAnnotation {
    void dumpDescription(RawConfig &config) const {
        EnumAnnotation::dumpDescription(config);
        config.setValueByPath("Enum/0", "api_key");
        config.setValueByPath("EnumI18n/0", _("API Key"));
        config.setValueByPath("Enum/1", "app_access_key");
        config.setValueByPath("EnumI18n/1", _("App Key + Access Key"));
    }
};

struct OpenaiLanguageAnnotation : public EnumAnnotation {
    void dumpDescription(RawConfig &config) const {
        EnumAnnotation::dumpDescription(config);
        config.setValueByPath("Enum/0", "auto");
        config.setValueByPath("EnumI18n/0", _("Default (Auto)"));
        config.setValueByPath("Enum/1", "en");
        config.setValueByPath("EnumI18n/1", "English");
        config.setValueByPath("Enum/2", "zh");
        config.setValueByPath("EnumI18n/2", "中文");
    }
};

FCITX_CONFIGURATION(OpenAIAsrConfig,
    Option<std::string> baseUrl{
        this, "BaseUrl", _("接口地址"),
        "https://api.openai.com/v1"};

    Option<std::string> apiKey{
        this, "ApiKey", _("API 密钥"), ""};

    Option<std::string> model{
        this, "Model", _("语音模型"), "whisper-1"};

    Option<std::string, NoConstrain<std::string>,
           DefaultMarshaller<std::string>, OpenaiLanguageAnnotation>
        language{
            this, "Language", _("输出语言"), "auto"};

    Option<bool> llmEnabled{
        this, "LLMEnabled", _("LLM 后处理"), false};

    Option<std::string> llmModel{
        this, "LLMModel", _("后处理 LLM 模型"), ""};

    Option<std::string> llmSystemPrompt{
        this, "LLMSystemPrompt", _("后处理系统提示词"), ""};

    Option<bool> llmStream{
        this, "LLMStream", _("LLM 流式输出"), true};

    Option<bool> autoCommit{
        this, "AutoCommit", _("无 LLM 时自动上屏"), true};
);

FCITX_CONFIGURATION(VolcengineAsrConfig,
    Option<std::string> endpoint{
        this, "Endpoint", _("WebSocket 地址"),
        "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"};

    Option<std::string, NoConstrain<std::string>,
           DefaultMarshaller<std::string>, VolcengineAuthModeAnnotation>
        authMode{
            this, "AuthMode", _("认证模式"), "api_key"};

    Option<std::string> apiKey{
        this, "ApiKey", _("API 密钥"), ""};

    Option<std::string> appKey{
        this, "AppKey", _("App 密钥"), ""};

    Option<std::string> accessKey{
        this, "AccessKey", _("Access 密钥"), ""};

    Option<std::string> resourceId{
        this, "ResourceId", _("资源 ID"),
        "volc.seedasr.sauc.duration"};

    Option<int, IntConstrain> chunkMs{
        this, "ChunkMs", _("音频分片 (毫秒)"), 200,
        IntConstrain(100, 200)};

    Option<bool> enableITN{
        this, "EnableITN", _("ITN 逆文本标准化"), true};

    Option<bool> enablePunc{
        this, "EnablePunc", _("标点符号"), true};

    Option<bool> enableDDC{
        this, "EnableDDC", _("语义顺滑"), false};

    Option<bool> enableNonstream{
        this, "EnableNonstream", _("二次识别"), true};

    Option<int, IntConstrain> endWindowMs{
        this, "EndWindowMs", _("判停窗口 (毫秒)"), 800,
        IntConstrain(200, 3000)};
);

FCITX_CONFIGURATION(VoiceInputConfig,
    Option<std::string, NoConstrain<std::string>,
           DefaultMarshaller<std::string>, AsrBackendAnnotation>
        activeBackend{
            this, "ActiveBackend", _("当前 ASR 后端"), "openai"};

    Option<int, IntConstrain> vadThreshold{
        this, "VADThreshold", _("语音检测阈值 (%)"), 20,
        IntConstrain(0, 100)};

    Option<int, IntConstrain> silenceThresholdMs{
        this, "SilenceThresholdMs", _("静音检测阈值 (毫秒)"), 800,
        IntConstrain(100, 10000)};

    Option<int, IntConstrain> startFrames{
        this, "StartFrames", _("启动帧数"), 2,
        IntConstrain(1, 10)};

    Option<int, IntConstrain> preRollMs{
        this, "PreRollMs", _("预卷时长 (毫秒)"), 300,
        IntConstrain(0, 1000)};

    Option<int, IntConstrain> minSpeechMs{
        this, "MinSpeechMs", _("最短语音时长 (毫秒)"), 300,
        IntConstrain(100, 10000)};

    Option<int, IntConstrain> maxSpeechMs{
        this, "MaxSpeechMs", _("最长语音时长 (毫秒)"), 30000,
        IntConstrain(1000, 60000)};
);

} // namespace fcitx
