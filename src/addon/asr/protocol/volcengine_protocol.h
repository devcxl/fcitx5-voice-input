#pragma once

// 火山引擎（豆包）SAUC 协议的纯编解码辅助：无会话状态、无 I/O。
// 仅由 volcengine_asr.cpp include；WebSocket 发送/接收/关闭与 RecvStatus
// 等传输逻辑仍留在会话实现内。

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <fcitx-utils/log.h>
#include <json/json.h>
#include <zlib.h>

namespace fcitx {
namespace volcengine_protocol {

constexpr uint8_t kMsgFullClientRequest = 0x1;
constexpr uint8_t kMsgAudioOnlyRequest = 0x2;
constexpr uint8_t kMsgFullServerResponse = 0x9;
constexpr uint8_t kMsgError = 0xF;
constexpr uint8_t kFlagNoSequence = 0x0;
constexpr uint8_t kFlagPositiveSequence = 0x1;
constexpr uint8_t kFlagFinalNoSequence = 0x2;
constexpr uint8_t kFlagFinalSequence = 0x3;
constexpr uint8_t kSerializationNone = 0x0;
constexpr uint8_t kSerializationJson = 0x1;
constexpr uint8_t kCompressionNone = 0x0;
constexpr uint8_t kCompressionGzip = 0x1;
constexpr size_t kMaxDecompressedBytes = 16 * 1024 * 1024; // gzip 解压累计 16MB

struct ServerMessage {
    uint8_t type = 0;
    uint8_t flags = 0;
    std::string payload;
};

inline void AppendUint32Be(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

inline uint32_t ReadUint32Be(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

inline std::vector<uint8_t> GzipCompress(const uint8_t* data, size_t size) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return {};
    std::vector<uint8_t> out(deflateBound(&zs, size));
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    zs.avail_in = static_cast<uInt>(size);
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    if (deflate(&zs, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&zs);
        return {};
    }
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

inline std::vector<uint8_t> GzipCompress(const std::string& data) {
    return GzipCompress(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

inline std::string GzipDecompress(const uint8_t* data, size_t size) {
    z_stream zs{};
    if (inflateInit2(&zs, MAX_WBITS + 16) != Z_OK) return {};
    std::string out;
    std::array<char, 8192> buffer{};
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    zs.avail_in = static_cast<uInt>(size);
    int result = Z_OK;
    while (result == Z_OK) {
        zs.next_out = reinterpret_cast<Bytef*>(buffer.data());
        zs.avail_out = static_cast<uInt>(buffer.size());
        result = inflate(&zs, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END) {
            inflateEnd(&zs);
            return {};
        }
        out.append(buffer.data(), buffer.size() - zs.avail_out);
        // 限制累计解压输出，防止恶意/异常服务端耗尽内存
        if (out.size() > kMaxDecompressedBytes) {
            FCITX_WARN() << "[voice-input:volcengine] gzip output exceeds "
                         << kMaxDecompressedBytes << " bytes";
            inflateEnd(&zs);
            return {};
        }
    }
    inflateEnd(&zs);
    return out;
}

inline std::vector<uint8_t> BuildFrame(uint8_t messageType, uint8_t flags,
                                       uint8_t serialization, uint8_t compression,
                                       const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    frame.reserve(8 + payload.size());
    frame.push_back(0x11);
    frame.push_back(static_cast<uint8_t>((messageType << 4) | flags));
    frame.push_back(static_cast<uint8_t>((serialization << 4) | compression));
    frame.push_back(0x00);
    AppendUint32Be(frame, static_cast<uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

inline std::string GenerateRequestId() {
    std::random_device rd;
    std::uniform_int_distribution<int> hexDist(0, 15);
    std::uniform_int_distribution<int> variantDist(8, 11);
    const char* hex = "0123456789abcdef";
    std::string id(36, '0');
    for (size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            id[i] = '-';
        } else {
            id[i] = hex[hexDist(rd)];
        }
    }
    id[14] = '4';
    id[19] = hex[variantDist(rd)];
    return id;
}

inline std::string MaskSecret(const std::string& secret) {
    if (secret.empty()) return "(none)";
    if (secret.size() <= 12) return "***";
    return secret.substr(0, 6) + "..." + secret.substr(secret.size() - 4);
}

inline std::string ExtractHeaderValue(const std::string& headers, const std::string& name) {
    size_t pos = headers.find(name);
    if (pos == std::string::npos) return "";
    pos += name.size();
    while (pos < headers.size() && headers[pos] == ' ') ++pos;
    size_t end = headers.find("\r\n", pos);
    if (end == std::string::npos) end = headers.find('\n', pos);
    return headers.substr(pos, end == std::string::npos ? end : end - pos);
}

inline bool ParseServerMessage(const std::vector<uint8_t>& frame, ServerMessage& msg) {
    if (frame.size() < 8) return false;
    uint8_t version = frame[0] >> 4;
    size_t headerSize = (frame[0] & 0x0f) * 4;
    if (version != 1 || frame.size() < headerSize + 4) return false;
    msg.type = frame[1] >> 4;
    msg.flags = frame[1] & 0x0f;
    uint8_t compression = frame[2] & 0x0f;
    size_t offset = headerSize;
    if (msg.type == kMsgFullServerResponse &&
        (msg.flags == kFlagPositiveSequence || msg.flags == kFlagFinalSequence)) {
        if (frame.size() < offset + 4) return false; offset += 4;
    } else if (msg.type == kMsgError) {
        if (frame.size() < offset + 4) return false; offset += 4;
    }
    if (frame.size() < offset + 4) return false;
    uint32_t payloadSize = ReadUint32Be(frame.data() + offset);
    offset += 4;
    if (frame.size() < offset + payloadSize) return false;
    if (compression == kCompressionGzip) {
        msg.payload = GzipDecompress(frame.data() + offset, payloadSize);
    } else {
        msg.payload.assign(reinterpret_cast<const char*>(frame.data() + offset), payloadSize);
    }
    return !msg.payload.empty() || payloadSize == 0;
}

inline std::string ExtractTextFromResult(const Json::Value& result) {
    if (result.isObject()) {
        std::string text = result.get("text", "").asString();
        if (!text.empty()) return text;
        const Json::Value& utterances = result["utterances"];
        if (utterances.isArray()) {
            std::string joined;
            for (Json::ArrayIndex i = 0; i < utterances.size(); ++i)
                joined += utterances[i].get("text", "").asString();
            return joined;
        }
    }
    if (result.isArray()) {
        std::string joined;
        for (Json::ArrayIndex i = 0; i < result.size(); ++i)
            joined += ExtractTextFromResult(result[i]);
        return joined;
    }
    return "";
}

inline bool ResponseHasDefiniteUtterance(const Json::Value& json) {
    const Json::Value& result = json["result"];
    if (!result.isObject()) return false;
    const Json::Value& utterances = result["utterances"];
    if (!utterances.isArray() || utterances.empty()) return false;
    for (Json::ArrayIndex i = 0; i < utterances.size(); ++i)
        if (!utterances[i].get("definite", false).asBool()) return false;
    return true;
}

inline Json::Value BuildRequestJson(const std::string& modelName, bool enableItN,
                                    bool enablePunc, bool enableDdc,
                                    bool enableNonstream, int endWindowMs) {
    Json::Value root;
    root["user"]["uid"] = "fcitx5-voice-input";
    root["audio"]["format"] = "pcm";
    root["audio"]["codec"] = "raw";
    root["audio"]["rate"] = 16000;
    root["audio"]["bits"] = 16;
    root["audio"]["channel"] = 1;
    root["request"]["model_name"] = modelName.empty() ? "bigmodel" : modelName.c_str();
    root["request"]["enable_itn"] = enableItN;
    root["request"]["enable_punc"] = enablePunc;
    root["request"]["enable_ddc"] = enableDdc;
    root["request"]["show_utterances"] = true;
    root["request"]["result_type"] = "full";
    root["request"]["enable_nonstream"] = enableNonstream;
    root["request"]["end_window_size"] = endWindowMs;
    return root;
}

inline std::string JsonToString(const Json::Value& json) {
    return Json::FastWriter().write(json);
}

} // namespace volcengine_protocol
} // namespace fcitx
