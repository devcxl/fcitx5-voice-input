#pragma once

// OpenAI Realtime 协议的纯构建/解析辅助：无会话状态、无 I/O。
// 仅由 realtime_asr.cpp include；发送/接收/关闭等传输逻辑仍留在会话实现内。

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <json/json.h>

#include "../utils/base64.h"

namespace fcitx {
namespace realtime_protocol {

// 16k → 24k 线性插值（比例 3/2）。输出样本数 = floor(1.5 * N)。
// 对每个输出下标 j，其对应的源位置 pos = j * 2/3，在相邻源样本间线性插值。
inline void Upsample16kTo24k(const std::vector<float>& in, std::vector<float>& out) {
    out.clear();
    if (in.empty()) return;
    size_t outCount = in.size() * 3 / 2;
    if (outCount == 0) return;
    out.reserve(outCount);
    for (size_t j = 0; j < outCount; ++j) {
        double pos = j * (2.0 / 3.0);
        size_t i = static_cast<size_t>(pos);
        double frac = pos - static_cast<double>(i);
        float a = in[i];
        float b = (i + 1 < in.size()) ? in[i + 1] : a;  // 末端 clamp
        out.push_back(a + static_cast<float>((b - a) * frac));
    }
}

/// 从服务端 error 事件中提取可读信息（issue #44：原实现完全忽略 error）。
/// OpenAI Realtime 的 error 事件形如
/// {"type":"error","error":{"type":"...","code":"...","message":"..."}}。
inline std::string DescribeError(const Json::Value& json) {
    const Json::Value& error = json["error"];
    if (error.isObject()) {
        const std::string message = error.get("message", "").asString();
        if (!message.empty()) return message;
        const std::string code = error.get("code", "").asString();
        if (!code.empty()) return code;
        const std::string type = error.get("type", "").asString();
        if (!type.empty()) return type;
    }
    const std::string message = json.get("message", "").asString();
    return message;
}

inline std::string JsonToString(const Json::Value& json) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, json);
}

// input_audio_buffer.append 事件：24k int16 PCM → base64。
inline std::string BuildAppendEvent(const std::vector<int16_t>& pcm24k) {
    Json::Value ev;
    ev["type"] = "input_audio_buffer.append";
    std::vector<uint8_t> bytes(pcm24k.size() * 2);
    std::memcpy(bytes.data(), pcm24k.data(), bytes.size());
    ev["audio"] = Base64Encode(bytes.data(), bytes.size());
    return JsonToString(ev);
}

inline std::string BuildCommitEvent() {
    Json::Value ev;
    ev["type"] = "input_audio_buffer.commit";
    return JsonToString(ev);
}

// session.update 事件：可选的语言/模型提示。
inline std::string BuildSessionUpdate(const std::string& language,
                                      const std::string& modelName) {
    Json::Value ev;
    ev["type"] = "session.update";
    if (!language.empty()) {
        ev["session"]["transcription"]["language"] = language;
    }
    if (!modelName.empty()) {
        ev["session"]["transcription"]["model"] = modelName;
    }
    return JsonToString(ev);
}

} // namespace realtime_protocol
} // namespace fcitx
