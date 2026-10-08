#pragma once

// OpenAI Realtime 协议的纯构建/解析辅助：无会话状态、无 I/O。
// 仅由 realtime_asr.cpp include；发送/接收/关闭等传输逻辑仍留在会话实现内。

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <json/json.h>

#include "../utils/base64.h"

namespace fcitx {
namespace realtime_protocol {

enum class EventType { Ignored, Delta, Completed, Failed, Committed, Error };

struct ServerEvent {
    EventType type = EventType::Ignored;
    std::string text;
    std::string itemId;
    std::string error;
};

inline bool IsOptionalString(const Json::Value& object, const char* field) {
    const auto& value = object[field];
    return value.isNull() || value.isString();
}

inline bool ValidateError(const Json::Value& json) {
    if (json.isMember("error")) {
        const auto& error = json["error"];
        if (!error.isObject()) return false;
        for (const char* field : {"message", "code", "type"}) {
            if (!IsOptionalString(error, field)) return false;
        }
    }
    return IsOptionalString(json, "message");
}

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

// Validate before conversion. JsonCpp defaults cover missing fields, not bad
// types. Unknown event kinds are ignored for forward-compatible extensions.
inline bool ParseServerEvent(const std::string& payload, ServerEvent& event) {
    Json::Value json;
    Json::Reader reader;
    if (!reader.parse(payload, json) || !json.isObject() ||
        !json["type"].isString()) return false;
    const std::string type = json["type"].asString();
    ServerEvent decoded;
    if (type == "conversation.item.input_audio_transcription.delta") {
        decoded.type = EventType::Delta;
        // OpenAI's delta is optional/nullable; null means no text, not failure.
        if (!IsOptionalString(json, "delta")) return false;
        decoded.text = json.get("delta", "").asString();
    } else if (type == "conversation.item.input_audio_transcription.completed") {
        decoded.type = EventType::Completed;
        if (!json["transcript"].isString()) return false;
        decoded.text = json["transcript"].asString();
    } else if (type == "conversation.item.input_audio_transcription.failed") {
        decoded.type = EventType::Failed;
    } else if (type == "input_audio_buffer.committed") {
        decoded.type = EventType::Committed;
    } else if (type == "error") {
        decoded.type = EventType::Error;
    }
    if (decoded.type != EventType::Ignored) {
        // Missing IDs retain the legacy commit-tracker fallback; present IDs
        // must be strings and must never be coerced from scalar JSON values.
        if (json.isMember("item_id") && !json["item_id"].isString()) return false;
        decoded.itemId = json.get("item_id", "").asString();
    }
    if (decoded.type == EventType::Failed || decoded.type == EventType::Error) {
        if (!ValidateError(json)) return false;
        decoded.error = DescribeError(json);
    }
    event = std::move(decoded);
    return true;
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
