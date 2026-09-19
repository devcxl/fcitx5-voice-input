#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

#include <curl/curl.h>
#include <fcitx-utils/log.h>

#include "ws_deadline.h"

namespace fcitx {

/// 发送一条完整的 WS 消息（TEXT / BINARY）。
///
/// 语义：
/// - `curl_ws_send` 允许分多次发完同一帧（后续调用需传相同 flags），本函数内部按
///   `sent` 推进直至消息发完。
/// - `CURLE_AGAIN`（socket 暂不可写）时短暂退避后重试，但受 `deadline` 约束：
///   到期即按发送失败返回，不再无限重试。
/// - `abort.Requested()` 置位立即返回（中止优先于发送完成）。
/// - 返回 false 表示未完整发出（中止 / 超时 / 传输错误），调用方需走既有失败分支
///   （重连或兜底终态）。
inline bool SendWsMessage(CURL* curl, const uint8_t* data, size_t size,
                          unsigned int flags, const WsAbort& abort,
                          const char* logTag,
                          WsDeadline deadline = WsDeadline{}) {
    if (!curl) return false;
    const uint8_t* ptr = data;
    size_t remaining = size;
    while (remaining > 0) {
        if (abort.Requested()) return false;

        size_t sent = 0;
        const CURLcode result =
            curl_ws_send(curl, ptr, remaining, &sent, 0, flags);
        if (result == CURLE_AGAIN) {
            if (deadline.Expired()) {
                FCITX_ERROR() << logTag << " WS send timeout after "
                              << deadline.Budget().count() << "ms (" << remaining
                              << " bytes pending): peer is not reading";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (result != CURLE_OK) {
            FCITX_ERROR() << logTag
                          << " curl_ws_send failed: " << curl_easy_strerror(result);
            return false;
        }
        if (sent == 0) {
            // 未报错但也没有进展：退避，避免在预算内空转烧 CPU
            if (deadline.Expired()) {
                FCITX_ERROR() << logTag << " WS send stalled (" << remaining
                              << " bytes pending)";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        ptr += sent;
        remaining -= sent;
    }
    return true;
}

inline bool SendWsMessage(CURL* curl, const std::string& data, unsigned int flags,
                          const WsAbort& abort, const char* logTag,
                          WsDeadline deadline = WsDeadline{}) {
    return SendWsMessage(curl, reinterpret_cast<const uint8_t*>(data.data()),
                         data.size(), flags, abort, logTag, deadline);
}

} // namespace fcitx
