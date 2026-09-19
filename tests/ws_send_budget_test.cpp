// WS 发送预算回归测试（GitHub issue #42）。
//
// 背景：三个流式 WS 后端的发送循环在 `curl_ws_send` 返回 `CURLE_AGAIN` 时只做
// `sleep(10ms)` + 重试，只检查 `cancelled`、不检查 `finished`，也没有 wall-clock
// 截止时间。上游不读 socket（TCP 窗口关闭：卡死/限流/反代 hang）时循环永不退出，
// 而 `End()` 只置 `finished` 不置 `cancelled`，于是 End 路径一旦卡住就永远出不来：
// worker 线程在 `SessionReaper::JoinWithTimeout(15s)` 超时后被 detach 泄漏，且该
// 会话永不产生终态，保序缓冲会把后续所有语音段的结果一起扣住。
//
// 本测试用本地假 WS 服务端在握手后**不再读取 socket** 并收紧接收缓冲，迫使发送
// 窗口关闭，验证：
//   1. `SendWsMessage` 在预算内失败（不无限重试）；
//   2. 真实 `RealtimeAsrSession` 的 `End()` 在有界时间内退出；
//   3. `JoinWithTimeout` 成功回收（不超时、不 detach）。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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

#include "asr/mistral_asr.h"
#include "asr/realtime_asr.h"
#include "asr/utils/ws_frame_sender.h"
#include "asr/volcengine_asr.h"

namespace {

using namespace std::chrono_literals;
using fcitx::SendWsMessage;
using fcitx::WsDeadline;
using fcitx::WsAbort;

bool Check(bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

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

bool LibcurlSupportsWebSocket() {
    curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (!info || !info->protocols) return false;
    for (const char* const* p = info->protocols; *p; ++p) {
        if (std::strcmp(*p, "ws") == 0) return true;
    }
    return false;
}

/// 假 WS 服务端：完成握手后**停止读取 socket**（并收紧接收缓冲），模拟上游卡死。
class NonReadingWsServer {
public:
    ~NonReadingWsServer() { Stop(); }

    bool Start() {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) return false;
        int one = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            return false;
        socklen_t len = sizeof(addr);
        if (getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
            return false;
        port_ = ntohs(addr.sin_port);
        if (listen(listenFd_, 4) != 0) return false;
        thread_ = std::thread([this] { Serve(); });
        return true;
    }

    void Stop() {
        stopping_.store(true);
        if (listenFd_ >= 0) {
            shutdown(listenFd_, SHUT_RDWR);
            close(listenFd_);
            listenFd_ = -1;
        }
        CloseClient();
        if (thread_.joinable()) thread_.join();
    }

    std::string Url() const {
        return "ws://127.0.0.1:" + std::to_string(port_) + "/";
    }

    void CloseClient() {
        if (clientFd_ >= 0) {
            shutdown(clientFd_, SHUT_RDWR);
            close(clientFd_);
            clientFd_ = -1;
        }
    }

private:
    void Serve() {
        clientFd_ = accept(listenFd_, nullptr, nullptr);
        if (clientFd_ < 0) return;

        // 收紧接收窗口，让发送方迅速遇到不可写
        int rcvbuf = 4096;
        setsockopt(clientFd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        std::string request;
        std::array<char, 2048> buf{};
        while (request.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = read(clientFd_, buf.data(), buf.size());
            if (n <= 0) return;
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
        const std::string response =
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
        if (write(clientFd_, response.data(), response.size()) < 0) return;

        // 握手完成后不再读取 socket：客户端发送最终会被窗口关闭挡住
        while (!stopping_.load()) {
            std::this_thread::sleep_for(50ms);
        }
        CloseClient();
    }

    int listenFd_ = -1;
    std::atomic<int> clientFd_{-1};
    uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

// ── 单元：SendWsMessage 的预算语义 ─────────────────────────────

/// 用一个「接收方不读」的真实 socket 对验证：超出预算即失败，且在预算附近返回。
bool TestSendBudgetBoundsRetryLoop() {
    NonReadingWsServer server;
    if (!Check(server.Start(), "server must listen")) return false;

    CURL* curl = curl_easy_init();
    if (!Check(curl != nullptr, "curl must init")) return false;
    curl_easy_setopt(curl, CURLOPT_URL, server.Url().c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    const bool connected =
        Check(curl_easy_perform(curl) == CURLE_OK, "WS connect must succeed");

    bool ok = connected;
    if (connected) {
        // 一次性发送远超接收窗口的数据：上行必然遇到 CURLE_AGAIN
        const std::string payload(8 * 1024 * 1024, 'x');
        std::atomic<bool> cancelled{false};
        std::atomic<bool> finished{false};
        const auto start = std::chrono::steady_clock::now();
        const bool sent =
            SendWsMessage(curl, payload, CURLWS_TEXT,
                          WsAbort::CancelOrFinished(cancelled, finished),
                          "[test]", WsDeadline(1500ms));
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        std::cerr << "  send budget: sent=" << sent
                  << " elapsed=" << elapsed.count() << "ms\n";
        ok = Check(!sent, "sending into a non-reading peer must fail") && ok;
        ok = Check(elapsed >= 1500ms && elapsed < 6000ms,
                   "the send must give up at its budget, not retry forever") && ok;
    }
    server.CloseClient();
    server.Stop();
    curl_easy_cleanup(curl);
    return ok;
}

/// 取消优先：即使接收方还在读，cancelFlag 置位后发送立即返回 false。
bool TestCancelFlagAbortsSend() {
    CURL* curl = curl_easy_init();
    if (!Check(curl != nullptr, "curl must init")) return false;

    std::atomic<bool> cancelled{true};
    std::atomic<bool> finished{false};
    const std::string payload(1024, 'y');
    const bool sent = SendWsMessage(curl, payload, CURLWS_TEXT,
                                    WsAbort::CancelOnly(cancelled), "[test]",
                                    WsDeadline(1s));
    curl_easy_cleanup(curl);
    return Check(!sent, "a cancelled session must not send");
}

// ── 端到端：上游不读 socket 时 End() 有界返回且可回收 ──────────

/// 通用：End() 后 worker 必须在 reaper 的 15s join 之前收敛，并交出终态。
/// 三个流式后端在「上游不读 socket」时都应满足。
bool CheckEndConverges(const std::shared_ptr<fcitx::AsrSession>& session,
                       std::atomic<bool>& gotFinal, const std::string& label) {
    const auto start = std::chrono::steady_clock::now();
    session->End();

    // SessionReaper 用的是 15s；这里用略小的超时验证「有界且不超时」
    const bool joined = fcitx::WaitForWorkerCompletion(
        session->GetState()->workerDone, 14s);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    std::this_thread::sleep_for(200ms);
    session->JoinWithTimeout(2s);

    std::cerr << "  " << label << ": joined=" << joined
              << " final=" << gotFinal.load() << " end->done=" << elapsed.count()
              << "ms\n";
    return Check(joined, label + ": End must not hang when the peer stops reading; "
                                 "the worker must finish within the send budget "
                                 "(well under the 15s reaper join)") &&
           Check(gotFinal.load(),
                 label + ": a stalled End must still publish a terminal result") &&
           Check(elapsed < 14s, label + ": End must return within a bounded time");
}

bool TestRealtimeEndIsBoundedWhenPeerStopsReading() {
    NonReadingWsServer server;
    if (!Check(server.Start(), "server must listen")) return false;

    fcitx::AsrEngine::Config config;
    config.apiEndpoint = server.Url();
    config.apiKey = "test-key";
    config.modelName = "gpt-live-transcribe";

    auto session = std::make_shared<fcitx::RealtimeAsrSession>(config, nullptr, 1);
    std::atomic<bool> gotFinal{false};
    session->SetResultCallback(
        [&gotFinal](const std::string&, bool isFinal, uint64_t) {
            if (isFinal) gotFinal.store(true);
        });

    session->StartWorker();

    // 喂入足够音频让发送循环真的向服务端推流（每个 chunk 约 100ms @24k）
    std::vector<float> audio(1600 * 40, 0.01f);
    for (int i = 0; i < 40; ++i) {
        session->FeedAudio(audio.data() + i * 1600, 1600);
        std::this_thread::sleep_for(10ms);
    }

    const bool ok =
        CheckEndConverges(session, gotFinal, "realtime non-reading peer");
    server.CloseClient();
    server.Stop();
    return ok;
}

bool TestMistralEndIsBoundedWhenPeerStopsReading() {
    NonReadingWsServer server;
    if (!Check(server.Start(), "server must listen")) return false;

    fcitx::AsrEngine::Config config;
    config.apiEndpoint = server.Url();
    config.apiKey = "test-key";
    config.modelName = "voxtral-mini-transcribe-realtime-2602";

    auto session = std::make_shared<fcitx::MistralAsrSession>(config, nullptr, 1);
    std::atomic<bool> gotFinal{false};
    session->SetResultCallback(
        [&gotFinal](const std::string&, bool isFinal, uint64_t) {
            if (isFinal) gotFinal.store(true);
        });

    session->StartWorker();

    std::vector<float> audio(1600 * 40, 0.01f);
    for (int i = 0; i < 40; ++i) {
        session->FeedAudio(audio.data() + i * 1600, 1600);
        std::this_thread::sleep_for(10ms);
    }

    const bool ok = CheckEndConverges(session, gotFinal, "mistral non-reading peer");
    server.CloseClient();
    server.Stop();
    return ok;
}

bool TestVolcengineEndIsBoundedWhenPeerStopsReading() {
    NonReadingWsServer server;
    if (!Check(server.Start(), "server must listen")) return false;

    fcitx::AsrEngine::Config config;
    config.apiEndpoint = server.Url();
    config.apiKey = "test-key";
    config.modelName = "bigmodel";

    auto session =
        std::make_shared<fcitx::VolcengineAsrSession>(config, nullptr, 1);
    std::atomic<bool> gotFinal{false};
    session->SetResultCallback(
        [&gotFinal](const std::string&, bool isFinal, uint64_t) {
            if (isFinal) gotFinal.store(true);
        });

    session->StartWorker();

    std::vector<float> audio(1600 * 40, 0.01f);
    for (int i = 0; i < 40; ++i) {
        session->FeedAudio(audio.data() + i * 1600, 1600);
        std::this_thread::sleep_for(10ms);
    }

    const bool ok =
        CheckEndConverges(session, gotFinal, "volcengine non-reading peer");
    server.CloseClient();
    server.Stop();
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = TestSendBudgetBoundsRetryLoop() && ok;
    ok = TestCancelFlagAbortsSend() && ok;

    if (!LibcurlSupportsWebSocket()) {
        std::cerr << "SKIP: libcurl has no WebSocket support on this platform; "
                     "send-budget cases need a real WS connection.\n";
        return ok ? 0 : 1;
    }
    ok = TestRealtimeEndIsBoundedWhenPeerStopsReading() && ok;
    ok = TestMistralEndIsBoundedWhenPeerStopsReading() && ok;
    ok = TestVolcengineEndIsBoundedWhenPeerStopsReading() && ok;
    return ok ? 0 : 1;
}
