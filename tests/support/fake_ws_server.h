#pragma once

// 测试共用的假 WebSocket 服务端与帧编码工具。
//
// 从 ws_frame_reassembly_test.cpp 提取，供「分片重组」与「非法响应」两类
// 回归测试复用。仅使用本地 127.0.0.1 真实 socket，不接触任何真实云服务或凭据。
//
// 握手/分片语义保持不变；投递后保持连接，避免意外 EOF/重连掩盖解析结果。
// 失败安全：
//   * 收发有界：SO_RCVTIMEO/SO_SNDTIMEO + 轮询 stopping_，任何阻塞调用都不会
//     无限等待；
//   * 发送用 MSG_NOSIGNAL 并处理短写/EINTR/EAGAIN，对端关闭不会触发 SIGPIPE；
//   * Stop() 通过 shutdown() 打断 accept() 与 read()，join 之后才 close() fd，
//     避免 close 与 fd 复用竞态；监听 fd 在服务线程内捕获为局部值。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace fcitxws {

using namespace std::chrono_literals;

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

inline std::string Base64(const uint8_t* d, size_t n) {
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

inline std::vector<uint8_t> EncodeDataFrame(uint8_t firstByte,
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

inline std::vector<uint8_t> EncodeTextFrame(const std::string& payload) {
    return EncodeDataFrame(0x81,
                           std::vector<uint8_t>(payload.begin(), payload.end()));
}

inline std::vector<uint8_t> EncodeBinaryFrame(const std::vector<uint8_t>& payload) {
    return EncodeDataFrame(0x82, payload);
}

inline void AppendUint32Be(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t((value >> 24) & 0xff));
    out.push_back(uint8_t((value >> 16) & 0xff));
    out.push_back(uint8_t((value >> 8) & 0xff));
    out.push_back(uint8_t(value & 0xff));
}

/// 火山引擎 bigmodel 服务端响应帧：带 sequence 的 full server response。
/// 布局：[0x11][type|flags][ser|comp][0x00][sequence:4][payload size:4][payload]
inline std::vector<uint8_t> VolcengineFrame(uint8_t messageType, uint8_t flags,
                                            uint8_t serialization,
                                            uint8_t compression,
                                            uint32_t sequence,
                                            const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    frame.push_back(0x11);
    frame.push_back(uint8_t((messageType << 4) | flags));
    frame.push_back(uint8_t((serialization << 4) | compression));
    frame.push_back(0x00);
    AppendUint32Be(frame, sequence);
    AppendUint32Be(frame, static_cast<uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

inline std::vector<uint8_t> VolcengineResponseFrame(const std::string& json,
                                                    uint32_t sequence,
                                                    uint8_t flags = 0x3) {
    return VolcengineFrame(0x9, flags, 0x1, 0x0, sequence,
                           std::vector<uint8_t>(json.begin(), json.end()));
}

/// 假 WS 服务端：完成握手后把预置的原始字节按 writeSize 分片 + 间隔写出，
/// 之后保持连接直到 Stop()（避免客户端看到意外 EOF 触发重连）。
class FakeWsServer {
public:
    FakeWsServer(std::vector<uint8_t> response, size_t writeSize = 4096,
                 std::chrono::milliseconds gap = 0ms,
                 std::chrono::milliseconds settle = 150ms)
        : response_(std::move(response)),
          writeSize_(writeSize == 0 ? 1 : writeSize),
          gap_(gap),
          settle_(settle) {}

    ~FakeWsServer() { Stop(); }

    FakeWsServer(const FakeWsServer&) = delete;
    FakeWsServer& operator=(const FakeWsServer&) = delete;

    bool Start() {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) return false;
        int one = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            return false;
        socklen_t len = sizeof(addr);
        if (::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
            return false;
        port_ = ntohs(addr.sin_port);
        if (::listen(listenFd_, 4) != 0) return false;
        thread_ = std::thread([this] { Serve(); });
        return true;
    }

    void Stop() {
        stopping_.store(true);
        const int lfd = listenFd_;              // 捕获后不再读成员，直到 join 完成
        const int cfd = clientFd_.load();
        if (lfd >= 0) ::shutdown(lfd, SHUT_RDWR);   // 打断 accept()
        if (cfd >= 0) ::shutdown(cfd, SHUT_RDWR);   // 打断 recv()
        if (thread_.joinable()) thread_.join();
        // 服务线程不再使用这些 fd，此刻 close 无 fd 复用竞态。
        // accept() may have completed after the pre-join snapshot. Reload its
        // published fd now, when Serve can no longer change it.
        const int accepted = clientFd_.load();
        if (accepted >= 0) ::close(accepted);
        if (lfd >= 0) ::close(lfd);
        listenFd_ = -1;
        clientFd_.store(-1);
    }

    uint16_t Port() const { return port_; }
    std::string Url() const {
        return "ws://127.0.0.1:" + std::to_string(port_) + "/";
    }

private:
    bool ReadRequest(int fd, std::string& request) {
        std::array<char, 4096> buf{};
        while (!stopping_.load() &&
               request.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
            if (n > 0) {
                request.append(buf.data(), size_t(n));
                continue;
            }
            if (n == 0) return false;                 // 客户端关闭
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  // SO_RCVTIMEO
            return false;
        }
        return request.find("\r\n\r\n") != std::string::npos;
    }

    bool SendAll(int fd, const uint8_t* data, size_t size) {
        size_t off = 0;
        while (off < size) {
            if (stopping_.load()) return false;
            const ssize_t n =
                ::send(fd, data + off, size - off, MSG_NOSIGNAL);
            if (n > 0) {
                off += size_t(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            return false;
        }
        return true;
    }

    void SleepInterruptible(std::chrono::milliseconds total) {
        const auto deadline = std::chrono::steady_clock::now() + total;
        while (!stopping_.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
    }

    void DrainClient(int fd) {
        std::array<char, 4096> buf{};
        while (!stopping_.load()) {
            const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
            if (n > 0) continue;
            if (n == 0) return;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  // SO_RCVTIMEO
            return;
        }
    }

    void Serve() {
        const int lfd = listenFd_;  // 在服务线程内捕获，Stop 不 close 直至 join
        const int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) return;
        clientFd_.store(cfd);

        timeval rcv{0, 200000};
        ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
        timeval snd{0, 200000};
        ::setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));

        std::string request;
        if (!ReadRequest(cfd, request)) return;

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
        const std::string headers =
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nX-Tt-Logid: test-log-id\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        if (!SendAll(cfd, reinterpret_cast<const uint8_t*>(headers.data()),
                     headers.size())) {
            return;
        }

        if (settle_.count() > 0) SleepInterruptible(settle_);

        size_t off = 0;
        while (off < response_.size() && !stopping_.load()) {
            const size_t n = std::min(writeSize_, response_.size() - off);
            if (!SendAll(cfd, response_.data() + off, n)) break;
            off += n;
            if (gap_.count() > 0) SleepInterruptible(gap_);
        }

        DrainClient(cfd);
    }

    std::vector<uint8_t> response_;
    size_t writeSize_;
    std::chrono::milliseconds gap_;
    std::chrono::milliseconds settle_;
    int listenFd_ = -1;                 // 仅 Start/Stop（同一线程）读写
    std::atomic<int> clientFd_{-1};     // Serve 写、Stop 读
    uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

} // namespace fcitxws
