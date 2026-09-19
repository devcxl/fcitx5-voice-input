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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
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
#include "asr/volcengine_asr.h"
#include "asr/utils/ws_frame_receiver.h"

namespace {

using namespace std::chrono_literals;
using fcitx::ReceiveWsMessage;
using fcitx::WsFrameReceiver;
using fcitx::WsMessageStatus;

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

// ── 测试用 WS 服务端 ─────────────────────────────────────────

/// 精简 SHA1（仅用于测试中的 WS 握手校验）。
struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    static uint32_t Rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }
    void Block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                   (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t t = Rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::array<uint8_t, 20> Digest(const std::string& s) {
        std::vector<uint8_t> msg(s.begin(), s.end());
        const uint64_t bits = uint64_t(msg.size()) * 8;
        msg.push_back(0x80);
        while (msg.size() % 64 != 56) msg.push_back(0);
        for (int i = 7; i >= 0; --i) msg.push_back(uint8_t(bits >> (i * 8)));
        for (size_t i = 0; i < msg.size(); i += 64) Block(&msg[i]);
        std::array<uint8_t, 20> out{};
        for (int i = 0; i < 5; ++i) {
            out[i * 4] = uint8_t(h[i] >> 24);
            out[i * 4 + 1] = uint8_t(h[i] >> 16);
            out[i * 4 + 2] = uint8_t(h[i] >> 8);
            out[i * 4 + 3] = uint8_t(h[i]);
        }
        return out;
    }
};

std::string Base64(const uint8_t* d, size_t n) {
    static const char* t =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = uint32_t(d[i]) << 16;
        if (i + 1 < n) v |= uint32_t(d[i + 1]) << 8;
        if (i + 2 < n) v |= uint32_t(d[i + 2]);
        o += t[(v >> 18) & 63];
        o += t[(v >> 12) & 63];
        o += (i + 1 < n) ? t[(v >> 6) & 63] : '=';
        o += (i + 2 < n) ? t[v & 63] : '=';
    }
    return o;
}

std::vector<uint8_t> EncodeDataFrame(uint8_t firstByte,
                                     const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    frame.push_back(firstByte);
    if (payload.size() < 126) {
        frame.push_back(uint8_t(payload.size()));
    } else if (payload.size() < 65536) {
        frame.push_back(126);
        frame.push_back(uint8_t(payload.size() >> 8));
        frame.push_back(uint8_t(payload.size() & 0xff));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(uint8_t((uint64_t(payload.size()) >> (i * 8)) & 0xff));
        }
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::vector<uint8_t> EncodeTextFrame(const std::string& payload) {
    return EncodeDataFrame(
        0x81, std::vector<uint8_t>(payload.begin(), payload.end()));
}

std::vector<uint8_t> EncodeBinaryFrame(const std::vector<uint8_t>& payload) {
    return EncodeDataFrame(0x82, payload);
}

void AppendUint32Be(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t((value >> 24) & 0xff));
    out.push_back(uint8_t((value >> 16) & 0xff));
    out.push_back(uint8_t((value >> 8) & 0xff));
    out.push_back(uint8_t(value & 0xff));
}

/// 火山引擎 bigmodel 服务端响应帧：带 sequence 的 full server response。
/// 布局：[0x11][type|flags][ser|comp][0x00][sequence:4][payload size:4][payload]
std::vector<uint8_t> VolcengineResponseFrame(const std::string& json,
                                             uint32_t sequence,
                                             uint8_t flags = 0x3 /* final */) {
    std::vector<uint8_t> frame;
    frame.push_back(0x11);
    frame.push_back(uint8_t((0x9 << 4) | flags));
    frame.push_back(uint8_t(0x10));  // JSON, no compression
    frame.push_back(0x00);
    AppendUint32Be(frame, sequence);
    const auto payload = std::vector<uint8_t>(json.begin(), json.end());
    AppendUint32Be(frame, static_cast<uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

/// 假 WS 服务端：完成握手后把预置的原始字节按 writeSize 分片 + 间隔写出。
class FakeWsServer {
public:
    FakeWsServer(std::vector<uint8_t> response,
                 size_t writeSize,
                 std::chrono::milliseconds gap)
        : response_(std::move(response)), writeSize_(writeSize), gap_(gap) {}

    ~FakeWsServer() { Stop(); }

    bool Start() {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) return false;
        int one = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return false;
        }
        socklen_t len = sizeof(addr);
        if (getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return false;
        }
        port_ = ntohs(addr.sin_port);
        if (listen(listenFd_, 4) != 0) return false;
        thread_ = std::thread([this] { Serve(); });
        return true;
    }

    void Stop() {
        stopping_ = true;
        if (listenFd_ >= 0) {
            shutdown(listenFd_, SHUT_RDWR);
            close(listenFd_);
            listenFd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    uint16_t Port() const { return port_; }
    std::string Url() const {
        return "ws://127.0.0.1:" + std::to_string(port_) + "/";
    }

private:
    void Serve() {
        const int cfd = accept(listenFd_, nullptr, nullptr);
        if (cfd < 0) return;

        std::string request;
        std::array<char, 4096> buf{};
        while (request.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = read(cfd, buf.data(), buf.size());
            if (n <= 0) {
                close(cfd);
                return;
            }
            request.append(buf.data(), size_t(n));
        }

        std::string key;
        const size_t pos = request.find("Sec-WebSocket-Key:");
        if (pos != std::string::npos) {
            size_t start = pos + std::strlen("Sec-WebSocket-Key:");
            while (start < request.size() && request[start] == ' ') ++start;
            const size_t end = request.find("\r\n", start);
            key = request.substr(start, end - start);
        }
        Sha1 sha;
        const auto digest = sha.Digest(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        const std::string accept = Base64(digest.data(), digest.size());
        const std::string responseHeaders =
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nX-Tt-Logid: test-log-id\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        if (write(cfd, responseHeaders.data(), responseHeaders.size()) < 0) {
            close(cfd);
            return;
        }

        // 让客户端先发完握手后的事件（session.update / 请求帧 / 音频），
        // 不做解析——本测试只验证服务端到客户端方向的分片重组。
        std::this_thread::sleep_for(150ms);

        size_t off = 0;
        while (off < response_.size() && !stopping_) {
            const size_t n = std::min(writeSize_, response_.size() - off);
            const ssize_t written = write(cfd, response_.data() + off, n);
            if (written <= 0) break;
            off += size_t(written);
            std::this_thread::sleep_for(gap_);
        }
        std::this_thread::sleep_for(300ms);
        close(cfd);
    }

    std::vector<uint8_t> response_;
    size_t writeSize_;
    std::chrono::milliseconds gap_;
    int listenFd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

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
