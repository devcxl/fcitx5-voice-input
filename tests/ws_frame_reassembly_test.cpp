// WS 分片重组回归测试（GitHub issue #41）。
//
// 背景：三个流式 WS 后端（OpenAI Realtime / Mistral Realtime / Volcengine）曾把
// 帧累积缓冲放在接收函数的局部变量里并在每次调用开头 clear()。当一条事件消息超过
// 一个 TCP 段时，curl_ws_recv 先在 socket 暂无数据时返回 CURLE_AGAIN，已收前缀被
// 丢弃，下一轮拿到的是同一消息的后半段，JSON 解析必然失败并被静默跳过。
//
// 本测试同时覆盖两个层面：
//   1. WsFrameReceiver 单测（边界、超限丢弃、类型过滤、CLOSE）；
//   2. 真实 AsrSession + 假 WS 服务端的端到端分片投递（Realtime TEXT / Volcengine BINARY）。
//
// 假服务端与帧编码器已提取到 support/fake_ws_server.h，供非法响应回归测试复用。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include "asr/realtime_asr.h"
#include "asr/utils/ws_frame_receiver.h"
#include "asr/volcengine_asr.h"
#include "support/fake_ws_server.h"

namespace {

using namespace std::chrono_literals;
using fcitx::ReceiveWsMessage;
using fcitx::WsFrameReceiver;
using fcitx::WsMessageStatus;
using fcitxws::EncodeBinaryFrame;
using fcitxws::EncodeTextFrame;
using fcitxws::FakeWsServer;
using fcitxws::VolcengineResponseFrame;

bool Check(bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

std::string Repeat(char c, size_t n) { return std::string(n, c); }

/// 本测试需要一个带 WebSocket 支持的 libcurl。Ubuntu 24.04 / Debian 12 的
/// 系统 libcurl（8.5 / 7.88）编译时未启用 WS，这些平台上流式后端本身不可用，
/// 因此跳过依赖真实传输的用例，而不是把它们误报为失败。
bool LibcurlSupportsWebSocket() {
    curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (!info || !info->protocols) return false;
    for (const char* const* p = info->protocols; *p; ++p) {
        if (std::strcmp(*p, "ws") == 0) return true;
    }
    return false;
}

// ── WsFrameReceiver 单元用例 ──────────────────────────────────

bool TestWholeMessageInOneChunk() {
    WsFrameReceiver r(CURLWS_TEXT);
    const std::string msg = "hello";
    const auto st = r.OnChunk(CURLWS_TEXT, 0,
                              reinterpret_cast<const uint8_t*>(msg.data()),
                              msg.size());
    return Check(st == WsFrameReceiver::Status::Ok,
                 "one-shot message must complete") &&
           Check(r.AsString() == msg, "one-shot payload must be preserved");
}

bool TestSplitChunksAreReassembled() {
    WsFrameReceiver r(CURLWS_TEXT);
    const std::string msg = Repeat('x', 100);
    const auto* data = reinterpret_cast<const uint8_t*>(msg.data());
    if (!Check(r.OnChunk(CURLWS_TEXT, 70, data, 30) == WsFrameReceiver::Status::Again,
               "first fragment must wait for more data")) {
        return false;
    }
    if (!Check(r.OnChunk(CURLWS_TEXT, 40, data + 30, 30) == WsFrameReceiver::Status::Again,
               "middle fragment must wait for more data")) {
        return false;
    }
    return Check(r.OnChunk(CURLWS_TEXT, 0, data + 60, 40) == WsFrameReceiver::Status::Ok,
                 "final fragment must complete the message") &&
           Check(r.Bytes().size() == 100, "reassembled payload must be 100 bytes");
}

bool TestAgainDoesNotDiscardPrefix() {
    WsFrameReceiver r(CURLWS_TEXT);
    const size_t total = 24071;  // 与 issue #41 实测的 24KB 事件同量级
    const std::string msg = Repeat('a', total);
    const auto* data = reinterpret_cast<const uint8_t*>(msg.data());

    const size_t chunk = 4092;
    size_t off = 0;
    while (total - off > chunk) {
        // 每个片段之后模拟 socket 暂无数据：调用方会再次进入接收循环
        if (!Check(r.OnChunk(CURLWS_TEXT, static_cast<curl_off_t>(total - off - chunk),
                             data + off, chunk) == WsFrameReceiver::Status::Again,
                   "incomplete frame must report Again without dropping the prefix")) {
            return false;
        }
        off += chunk;
    }
    return Check(r.OnChunk(CURLWS_TEXT, 0, data + off, total - off) ==
                     WsFrameReceiver::Status::Ok,
                 "last fragment must complete the message") &&
           Check(r.AsString() == msg, "reassembled payload must match the original");
}

bool TestOversizedMessageIsSkippedAndBoundaryRecovered() {
    WsFrameReceiver r(CURLWS_TEXT, 10);
    const std::string big = Repeat('b', 25);
    const auto* data = reinterpret_cast<const uint8_t*>(big.data());
    if (!Check(r.OnChunk(CURLWS_TEXT, 10, data, 15) == WsFrameReceiver::Status::Again,
               "oversized message must keep consuming its fragments")) {
        return false;
    }
    if (!Check(r.OnChunk(CURLWS_TEXT, 0, data + 15, 10) == WsFrameReceiver::Status::Skipped,
               "oversized message must be skipped at its boundary")) {
        return false;
    }

    const std::string next = "next";
    return Check(r.OnChunk(CURLWS_TEXT, 0,
                           reinterpret_cast<const uint8_t*>(next.data()),
                           next.size()) == WsFrameReceiver::Status::Ok,
                 "the message after a skipped one must parse at the right boundary") &&
           Check(r.AsString() == next, "boundary recovery must not leak skipped bytes");
}

bool TestOtherMessageKindIsIgnored() {
    WsFrameReceiver r(CURLWS_TEXT);
    const std::string binary = "binary-payload";
    if (!Check(r.OnChunk(CURLWS_BINARY, 0,
                         reinterpret_cast<const uint8_t*>(binary.data()),
                         binary.size()) == WsFrameReceiver::Status::Again,
               "non-target frame must not complete the receiver")) {
        return false;
    }

    const std::string text = "text";
    return Check(r.OnChunk(CURLWS_TEXT, 0,
                           reinterpret_cast<const uint8_t*>(text.data()),
                           text.size()) == WsFrameReceiver::Status::Ok,
                 "target message after a foreign frame must still parse") &&
           Check(r.AsString() == text, "foreign frame bytes must not leak in");
}

bool TestFragmentedMessageAcrossFrames() {
    WsFrameReceiver r(CURLWS_TEXT);
    const std::string raw = "abcdef";
    const auto* data = reinterpret_cast<const uint8_t*>(raw.data());
    if (!Check(r.OnChunk(CURLWS_TEXT | CURLWS_CONT, 0, data, 2) ==
                   WsFrameReceiver::Status::Again,
               "a non-final WS fragment must not complete the message")) {
        return false;
    }
    if (!Check(r.OnChunk(CURLWS_TEXT | CURLWS_CONT, 0, data + 2, 2) ==
                   WsFrameReceiver::Status::Again,
               "a middle WS fragment must not complete the message")) {
        return false;
    }
    return Check(r.OnChunk(CURLWS_TEXT, 0, data + 4, 2) == WsFrameReceiver::Status::Ok,
                 "the final WS fragment must complete the message") &&
           Check(r.AsString() == "abcdef", "fragmented message must be reassembled");
}

bool TestCloseResetsReceiver() {
    WsFrameReceiver r(CURLWS_TEXT);
    const std::string partial = "partial";
    r.OnChunk(CURLWS_TEXT, 3,
              reinterpret_cast<const uint8_t*>(partial.data()), 4);
    return Check(r.OnChunk(CURLWS_CLOSE, 0, nullptr, 0) ==
                     WsFrameReceiver::Status::Closed,
                 "CLOSE must be reported") &&
           Check(r.Bytes().empty(), "CLOSE must reset any preserved prefix");
}

// ── 端到端：真实 RealtimeAsrSession + 分片 TEXT 帧 ────────────

bool TestSplitRealtimeDeltaIsReassembled() {
    const std::string delta = Repeat('a', 24000);
    const std::string payload =
        "{\"type\":\"conversation.item.input_audio_transcription.delta\","
        "\"delta\":\"" + delta + "\"}";
    FakeWsServer server(EncodeTextFrame(payload), 4096, 120ms);
    if (!Check(server.Start(), "fake WS server must listen")) return false;

    fcitx::AsrEngine::Config config;
    config.apiEndpoint = server.Url();
    config.apiKey = "test-key";
    config.modelName = "gpt-live-transcribe";

    auto session = std::make_shared<fcitx::RealtimeAsrSession>(config, nullptr, 1);
    std::atomic<size_t> maxPartial{0};
    session->SetResultCallback(
        [&maxPartial](const std::string& text, bool isFinal, uint64_t) {
            if (!isFinal && text.size() > maxPartial.load()) {
                maxPartial.store(text.size());
            }
        });

    session->StartWorker();

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline && maxPartial.load() < 24000) {
        std::this_thread::sleep_for(20ms);
    }

    session->Cancel();
    session->JoinWithTimeout(3s);
    server.Stop();

    const size_t observed = maxPartial.load();
    std::cerr << "  realtime split-delta: max partial len=" << observed << '\n';
    return Check(observed == 24000,
                 "a 24KB TEXT event split across TCP writes must be reassembled into "
                 "one delta (the prefix must not be dropped)");
}

/// 直接用 curl 驱动 ReceiveWsMessage，验证 24KB 级的 TEXT 消息在分片下完整重组。
bool TestReceiveWsMessageOverRealSocket() {
    const std::string delta = Repeat('a', 24000);
    const std::string payload =
        "{\"type\":\"conversation.item.input_audio_transcription.delta\","
        "\"delta\":\"" + delta + "\"}";
    FakeWsServer server(EncodeTextFrame(payload), 4096, 120ms);
    if (!Check(server.Start(), "fake WS server must listen")) return false;

    CURL* curl = curl_easy_init();
    bool ok = Check(curl != nullptr, "curl must init");
    std::string message;
    if (ok) {
        curl_easy_setopt(curl, CURLOPT_URL, server.Url().c_str());
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        ok = Check(curl_easy_perform(curl) == CURLE_OK, "WS connect must succeed");
    }

    if (ok) {
        WsFrameReceiver receiver(CURLWS_TEXT);
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto st = ReceiveWsMessage(curl, receiver, "[test]");
            if (st == WsMessageStatus::Ok) {
                message = receiver.AsString();
                break;
            }
            if (st == WsMessageStatus::Closed || st == WsMessageStatus::Error) break;
            std::this_thread::sleep_for(5ms);
        }
    }
    server.Stop();
    if (curl) curl_easy_cleanup(curl);

    return ok &&
           Check(message.size() > 24000,
                 "split message must be reassembled into one >24KB message") &&
           Check(message.find("\"delta\":\"aaa") != std::string::npos,
                 "reassembled message must start with the frame prefix");
}

// ── 端到端：真实 VolcengineAsrSession + 分片 BINARY 帧 ─────────

bool TestSplitVolcengineFrameIsReassembled() {
    const std::string text = Repeat('b', 24000);
    const std::string json = "{\"code\":20000000,\"result\":{\"text\":\"" + text + "\"}}";
    FakeWsServer server(EncodeBinaryFrame(VolcengineResponseFrame(json, 1)), 4096, 120ms);
    if (!Check(server.Start(), "fake WS server must listen")) return false;

    fcitx::AsrEngine::Config config;
    config.apiEndpoint = server.Url();
    config.apiKey = "test-key";
    config.modelName = "bigmodel";

    auto session = std::make_shared<fcitx::VolcengineAsrSession>(config, nullptr, 1);
    std::atomic<size_t> maxPartial{0};
    session->SetResultCallback(
        [&maxPartial](const std::string& text, bool isFinal, uint64_t) {
            if (!isFinal && text.size() > maxPartial.load()) {
                maxPartial.store(text.size());
            }
        });

    session->StartWorker();

    // 喂一段音频触发 WS 主循环（服务端在这之后回包）
    std::vector<float> audio(1600, 0.01f);
    session->FeedAudio(audio.data(), audio.size());

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline && maxPartial.load() < 24000) {
        std::this_thread::sleep_for(20ms);
    }

    session->Cancel();
    session->JoinWithTimeout(3s);
    server.Stop();

    const size_t observed = maxPartial.load();
    std::cerr << "  volcengine split-binary: max partial len=" << observed << '\n';
    return Check(observed == 24000,
                 "a 24KB BINARY response split across TCP writes must be parsed and "
                 "fire the partial callback");
}

} // namespace

int main() {
    bool ok = true;
    ok = TestWholeMessageInOneChunk() && ok;
    ok = TestSplitChunksAreReassembled() && ok;
    ok = TestAgainDoesNotDiscardPrefix() && ok;
    ok = TestOversizedMessageIsSkippedAndBoundaryRecovered() && ok;
    ok = TestOtherMessageKindIsIgnored() && ok;
    ok = TestFragmentedMessageAcrossFrames() && ok;
    ok = TestCloseResetsReceiver() && ok;

    if (!LibcurlSupportsWebSocket()) {
        std::cerr << "SKIP: libcurl has no WebSocket support on this platform "
                     "(Ubuntu 24.04 / Debian 12 system libcurl); protocol-level "
                     "cases are covered on distros with WS support.\n";
        return ok ? 0 : 1;
    }
    ok = TestReceiveWsMessageOverRealSocket() && ok;
    ok = TestSplitRealtimeDeltaIsReassembled() && ok;
    ok = TestSplitVolcengineFrameIsReassembled() && ok;
    return ok ? 0 : 1;
}
