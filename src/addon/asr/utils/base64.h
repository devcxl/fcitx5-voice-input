#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace fcitx {

/// Base64 编码，供 OpenAI Realtime（音频 base64）等流式路径复用。
/// 提取自 openai_asr.cpp 的匿名 namespace 实现，提升为公共函数。
std::string Base64Encode(const uint8_t* data, size_t len);

} // namespace fcitx
