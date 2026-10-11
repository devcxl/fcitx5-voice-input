#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <json/json.h>

#include "../utils/base64.h"

namespace fcitx::mistral_protocol {

// Provider-specific wire models never escape into session/pipeline state.
enum class EventType { Ignored, Created, Delta, Segment, Done, Error };
struct ServerEvent {
    EventType type = EventType::Ignored;
    std::string text;
    std::string error;
};

inline std::string Serialize(const Json::Value& event) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, event);
}

inline std::string BuildSessionUpdate(int delayMs) {
    Json::Value event;
    event["type"] = "session.update";
    event["session"]["audio_format"]["encoding"] = "pcm_s16le";
    event["session"]["audio_format"]["sample_rate"] = 16000;
    event["session"]["target_streaming_delay_ms"] = delayMs;
    return Serialize(event);
}

inline std::string BuildAppendEvent(const std::vector<int16_t>& pcm) {
    std::vector<uint8_t> bytes;
    bytes.reserve(pcm.size() * 2);
    for (int16_t sample : pcm) {
        const auto value = static_cast<uint16_t>(sample);
        bytes.push_back(static_cast<uint8_t>(value));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
    }
    Json::Value event;
    event["type"] = "input_audio.append";
    event["audio"] = Base64Encode(bytes.data(), bytes.size());
    return Serialize(event);
}

inline std::string BuildControlEvent(const char* type) {
    Json::Value event;
    event["type"] = type;
    return Serialize(event);
}

inline bool ParseServerEvent(const std::string& payload, ServerEvent& event) {
    Json::Value json;
    Json::Reader reader;
    if (!reader.parse(payload, json) || !json.isObject() ||
        !json["type"].isString()) return false;
    const auto type = json["type"].asString();
    ServerEvent decoded;
    if (type == "session.created" || type == "session.updated") {
        if (json.isMember("session") && !json["session"].isObject()) return false;
        if (type == "session.created") decoded.type = EventType::Created;
    } else if (type == "transcription.text.delta" ||
               type == "transcription.segment" || type == "transcription.done") {
        if (!json["text"].isString()) return false;
        decoded.type = type == "transcription.text.delta" ? EventType::Delta
                       : type == "transcription.segment" ? EventType::Segment
                                                        : EventType::Done;
        decoded.text = json["text"].asString();
    } else if (type == "error") {
        const auto& detail = json["error"];
        if (!detail.isObject()) return false;
        const auto& message = detail["message"];
        // Official message is Union[str, Dict[str, Any]], not always a string.
        if (!message.isString() && !message.isObject()) return false;
        if (detail.isMember("code") && !detail["code"].isInt()) return false;
        decoded.type = EventType::Error;
        decoded.error = "Mistral realtime error";
        if (detail.isMember("code"))
            decoded.error += " (code " + std::to_string(detail["code"].asInt()) + ")";
        // Do not echo untrusted error text: gateways can reflect credentials,
        // request data or transcripts. Numeric code retains safe diagnostics.
    }
    event = std::move(decoded);
    return true;
}

} // namespace fcitx::mistral_protocol
