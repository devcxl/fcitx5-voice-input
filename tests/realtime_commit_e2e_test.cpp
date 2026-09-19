// Realtime 会话行为回归测试（GitHub issue #44 的端到端形态）。
//
// 场景（复刻 issue 实测）：假服务端对每次 `input_audio_buffer.commit` 回一个 `error`
// （不回 `completed`）；在收到第 2 次 commit（End 提交）后立即补发最终 `completed`
// （内容 `最终文本`）。修复前 `commitsInFlight` 只增不减，最终 completed 被判定为
// 「非最终」，会话只能等兜底超时才收尾（issue 实测 30037ms）。

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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include "asr/realtime_asr.h"

namespace {

using namespace std::chrono_literals;

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

std::vector<uint8_t> EncodeTextFrame(const std::string& payload) {
    std::vector<uint8_t> frame;
    frame.push_back(0x81);  // FIN + TEXT
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

/// 假服务端：解析客户端 TEXT 帧里的 JSON，遇到 `input_audio_buffer.commit` 计数；
/// 第 1 次回 error（模拟对空 buffer 的 commit_empty），第 2 次（End）回最终 completed。
class CommitErrorWsServer {
public:
    ~CommitErrorWsServer() { Stop(); }

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
        if (clientFd_ >= 0) {
            shutdown(clientFd_, SHUT_RDWR);
            close(clientFd_);
            clientFd_ = -1;
        }
        if (listenFd_ >= 0) {
            shutdown(listenFd_, SHUT_RDWR);
            close(listenFd_);
            listenFd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    std::string Url() const {
        return "ws://127.0.0.1:" + std::to_string(port_) + "/";
    }

    bool SawCommit() const { return commitCount_.load() > 0; }

private:
    void SendText(const std::string& payload) {
        const auto frame = EncodeTextFrame(payload);
        (void)write(clientFd_, frame.data(), frame.size());
    }

    void Serve() {
        clientFd_ = accept(listenFd_, nullptr, nullptr);
        if (clientFd_ < 0) return;

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

        // 逐帧读取客户端消息（未掩码的客户端帧在真实客户端上会被掩码，这里按
        // 掩码位解析）。只关心 payload 中是否出现 input_audio_buffer.commit。
        std::vector<uint8_t> buffer;
        std::array<uint8_t, 4096> chunk{};
        while (!stopping_.load()) {
            const ssize_t n = read(clientFd_, chunk.data(), chunk.size());
            if (n <= 0) break;
            buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + n);

            while (buffer.size() >= 2) {
                const uint8_t b0 = buffer[0];
                const uint8_t b1 = buffer[1];
                const bool masked = (b1 & 0x80) != 0;
                uint64_t payloadLen = b1 & 0x7f;
                size_t headerLen = 2;
                if (payloadLen == 126) {
                    if (buffer.size() < 4) break;
                    payloadLen = (uint64_t(buffer[2]) << 8) | buffer[3];
                    headerLen = 4;
                } else if (payloadLen == 127) {
                    if (buffer.size() < 10) break;
                    payloadLen = 0;
                    for (int i = 0; i < 8; ++i) {
                        payloadLen = (payloadLen << 8) | buffer[2 + i];
                    }
                    headerLen = 10;
                }
                size_t maskLen = masked ? 4 : 0;
                if (buffer.size() < headerLen + maskLen + payloadLen) break;

                std::string payload(
                    reinterpret_cast<const char*>(buffer.data() + headerLen + maskLen),
                    size_t(payloadLen));
                if (masked) {
                    const uint8_t* mask = buffer.data() + headerLen;
                    for (size_t i = 0; i < payload.size(); ++i) {
                        payload[i] = char(uint8_t(payload[i]) ^ mask[i % 4]);
                    }
                }
                buffer.erase(buffer.begin(),
                             buffer.begin() + headerLen + maskLen + payloadLen);

                if ((b0 & 0x0f) != 0x1) continue;  // 只看 TEXT 帧
                if (payload.find("input_audio_buffer.commit") == std::string::npos) {
                    continue;
                }

                const int count = ++commitCount_;
                if (count == 1) {
                    // 周期 commit：回 error，不回 completed（复刻 issue 的场景）
                    SendText(
                        "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\","
                        "\"code\":\"input_audio_buffer_commit_empty\","
                        "\"message\":\"buffer is empty\"}}");
                } else {
                    // End commit：服务端**不回** committed ack（复刻 issue 实测场景），
                    // 直接补发最终 completed
                    std::this_thread::sleep_for(80ms);
                    SendText(
                        "{\"type\":\"conversation.item.input_audio_transcription.completed\","
                        "\"item_id\":\"item_end\",\"transcript\":\"最终文本\"}");
                }
            }
        }
    }

    int listenFd_ = -1;
    int clientFd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<int> commitCount_{0};
    std::thread thread_;
};

/// issue #44 的核心断言：error 之后的最终 completed 必须立即上报，不等待兜底超时。
bool TestFinalIsPublishedWithoutFallbackWait() {
    CommitErrorWsServer server;
    if (!Check(server.Start(), "fake server must listen")) return false;

    fcitx::AsrEngine::Config config;
    config.apiEndpoint = server.Url();
    config.apiKey = "test-key";
    config.modelName = "gpt-live-transcribe";
    // 5s commit 间隔：让第 1 次（周期）commit 在 End 之前触发
    config.commitIntervalMs = 1000;

    auto session = std::make_shared<fcitx::RealtimeAsrSession>(config, nullptr, 1);
    std::atomic<bool> gotFinal{false};
    std::string finalText;
    std::mutex finalMutex;
    session->SetResultCallback([&](const std::string& text, bool isFinal, uint64_t) {
        if (!isFinal) return;
        std::lock_guard<std::mutex> lock(finalMutex);
        finalText = text;
        gotFinal.store(true);
    });

    session->StartWorker();

    // 持续喂音频 ~2.5s，确保周期 commit 触发（服务端回 error）
    std::vector<float> audio(1600, 0.01f);
    for (int i = 0; i < 250; ++i) {
        session->FeedAudio(audio.data(), audio.size());
        std::this_thread::sleep_for(10ms);
        if (server.SawCommit()) break;
    }
    if (!Check(server.SawCommit(),
               "the periodic commit must have reached the fake server")) {
        session->Cancel();
        session->JoinWithTimeout(2s);
        server.Stop();
        return false;
    }

    const auto start = std::chrono::steady_clock::now();
    session->End();

    const auto deadline = start + 12s;
    while (std::chrono::steady_clock::now() < deadline && !gotFinal.load()) {
        std::this_thread::sleep_for(10ms);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    session->Cancel();
    session->JoinWithTimeout(3s);
    server.Stop();

    std::string published;
    {
        std::lock_guard<std::mutex> lock(finalMutex);
        published = finalText;
    }
    std::cerr << "  commit-error: final=" << gotFinal.load() << " text=\"" << published
              << "\" after " << elapsed.count() << "ms\n";
    return Check(gotFinal.load(), "the final result must be published") &&
           Check(elapsed < 3000ms,
                 "the final completed must be reported immediately after the End "
                 "commit, not after a fallback timeout (issue #44)") &&
           Check(published == "最终文本",
                 "the published text must be the server transcript");
}

} // namespace

int main() {
    if (!LibcurlSupportsWebSocket()) {
        std::cerr << "SKIP: libcurl has no WebSocket support on this platform.\n";
        return 0;
    }
    return TestFinalIsPublishedWithoutFallbackWait() ? 0 : 1;
}
