#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <fcitx-utils/log.h>

namespace fcitx {

/// WebSocket 单条消息重组器。
///
/// curl_ws_recv 在 socket 暂无数据时返回 CURLE_AGAIN，即使当前消息只到达了
/// 一部分 payload。因此累积缓冲必须跨调用保留：否则大帧跨 TCP 分片时前缀被
/// 丢弃，下一次调用拿到的是同一消息的后半段，JSON 解析必然失败并被静默跳过。
///
/// 用法：每次 curl_ws_recv 成功后在循环里调用 OnChunk()，直到返回 Ok（完整消息）、
/// Skipped（整条超限被丢弃）、Again（暂无更多数据，已收字节保留）或 Closed。
/// PING/PONG 由 libcurl 自动应答，不会出现在 curl_ws_recv 的结果中。
class WsFrameReceiver {
public:
    enum class Status {
        Ok,      // 完整消息已就绪（见 Bytes()/AsString()）
        Again,   // 消息未完整：已收字节保留，等待下次读取
        Skipped, // 本条消息超出上限，整条丢弃且解析边界已恢复
        Closed,  // 收到 CLOSE
        Error,   // 传输错误（半条消息丢弃）
    };

    static constexpr size_t kDefaultMaxMessageBytes = 16 * 1024 * 1024;

    explicit WsFrameReceiver(int targetFlags = CURLWS_TEXT,
                             size_t maxMessageBytes = kDefaultMaxMessageBytes)
        : targetFlags_(targetFlags), maxMessageBytes_(maxMessageBytes) {}

    /// 喂入 curl_ws_recv 解出的一个片段。参数直接取自 struct curl_ws_frame。
    Status OnChunk(int flags, curl_off_t bytesleft, const uint8_t* data,
                   size_t size);

    Status OnTransportError() {
        Reset();
        return Status::Error;
    }

    const std::vector<uint8_t>& Bytes() const { return buffer_; }

    std::string AsString() const {
        return std::string(buffer_.begin(), buffer_.end());
    }

    size_t MaxMessageBytes() const { return maxMessageBytes_; }

    void Reset() {
        buffer_.clear();
        state_ = State::Idle;
    }

    /// 当前是否处于「整条消息超限丢弃」状态（仅用于观测）。
    bool Dropping() const { return state_ == State::Discarding; }

private:
    enum class State {
        Idle,       // 两条消息之间
        Collecting, // 正在累积目标类型的消息
        Ignoring,   // 正在跳过非目标类型的消息（保持边界对齐）
        Discarding, // 目标消息超限，吞掉剩余分片
    };

    std::vector<uint8_t> buffer_;
    int targetFlags_;
    size_t maxMessageBytes_;
    State state_ = State::Idle;
};

inline WsFrameReceiver::Status WsFrameReceiver::OnChunk(int flags,
                                                       curl_off_t bytesleft,
                                                       const uint8_t* data,
                                                       size_t size) {
    if (flags & CURLWS_CLOSE) {
        Reset();
        return Status::Closed;
    }

    // 分片消息的每个 fragment 都携带消息类型位（TEXT/BINARY），可据此判断归属
    const bool isTarget = (flags & targetFlags_) != 0;
    if (state_ == State::Idle) {
        if (!isTarget) {
            // 非目标类型的消息：只有在本片段即结束时才无需跟踪后续边界
            state_ = State::Ignoring;
        } else {
            buffer_.clear();
            state_ = State::Collecting;
        }
    } else if (state_ == State::Collecting && !isTarget) {
        // 协议违规（分片消息中途切换类型）：放弃本条，重新对齐边界
        buffer_.clear();
        state_ = State::Ignoring;
    }

    if (state_ == State::Collecting && size > 0) {
        if (buffer_.size() + size > maxMessageBytes_) {
            buffer_.clear();
            state_ = State::Discarding;
        } else {
            buffer_.insert(buffer_.end(), data, data + size);
        }
    }

    // CURLWS_CONT 表示本 fragment 之后还有后续分片；bytesleft == 0 表示本 fragment
    // payload 已收完。两者同时成立才算整条消息结束。
    const bool messageEnd = (bytesleft == 0) && !(flags & CURLWS_CONT);
    if (!messageEnd) return Status::Again;

    const State finished = state_;
    state_ = State::Idle;
    if (finished == State::Collecting) return Status::Ok;
    if (finished == State::Discarding) return Status::Skipped;
    return Status::Again;
}

enum class WsMessageStatus { Ok, Again, Skipped, Closed, Error };

/// 从 connect-only 的 WS 连接读取一条完整消息。跨调用保持已收分片，
/// CURLE_AGAIN 不再丢弃前缀。errorOut 返回底层 CURLcode（仅 Error 时写入）。
inline WsMessageStatus ReceiveWsMessage(CURL* curl, WsFrameReceiver& receiver,
                                        const char* logTag,
                                        CURLcode* errorOut = nullptr) {
    std::array<uint8_t, 8192> buffer{};
    while (true) {
        size_t received = 0;
        // curl >= 8.2.0 将 curl_ws_recv 的 metap 参数 const 化；老版本（Debian 12 的
        // 7.88 等）为非 const 签名，需按编译期 curl 版本分支避免 -fpermissive 错误
#if LIBCURL_VERSION_NUM >= 0x080200
        const struct curl_ws_frame* meta = nullptr;
#else
        struct curl_ws_frame* meta = nullptr;
#endif
        CURLcode result =
            curl_ws_recv(curl, buffer.data(), buffer.size(), &received, &meta);
        if (result == CURLE_AGAIN) return WsMessageStatus::Again;
        if (result != CURLE_OK) {
            FCITX_ERROR() << logTag << " curl_ws_recv failed: "
                          << curl_easy_strerror(result);
            if (errorOut) *errorOut = result;
            receiver.OnTransportError();
            return WsMessageStatus::Error;
        }

        const int flags = meta ? meta->flags : 0;
        const curl_off_t bytesleft = meta ? meta->bytesleft : 0;
        switch (receiver.OnChunk(flags, bytesleft, buffer.data(), received)) {
        case WsFrameReceiver::Status::Ok:
            return WsMessageStatus::Ok;
        case WsFrameReceiver::Status::Skipped:
            FCITX_ERROR() << logTag << " WS message exceeds "
                          << receiver.MaxMessageBytes()
                          << " bytes, dropped";
            return WsMessageStatus::Skipped;
        case WsFrameReceiver::Status::Closed:
            return WsMessageStatus::Closed;
        case WsFrameReceiver::Status::Error:
            return WsMessageStatus::Error;
        case WsFrameReceiver::Status::Again:
            break;
        }
    }
}

} // namespace fcitx
