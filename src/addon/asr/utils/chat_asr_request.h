#pragma once

#include <string>

#include <json/json.h>

namespace fcitx {

/// 构造 OpenAI 兼容 Chat Completions 音频转录请求体
/// （DashScope qwen3-asr-flash / 小米 MiMo 等）。
/// language 为空或 "auto" 时不发送 language 字段；
/// enableItn=true 时附加 DashScope 扩展字段 asr_options.enable_itn；
/// 两者均无时整个 asr_options 字段省略。
Json::Value BuildChatAsrRequestBody(const std::string& model,
                                    const std::string& language,
                                    bool enableItn,
                                    const std::string& audioDataUrl);

} // namespace fcitx
