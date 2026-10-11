#pragma once

// 交互式 Mistral Realtime 假服务端（仅监听 127.0.0.1，不使用任何真实凭据）。
//
// 与 support/fake_ws_server.h 的「预置字节回放」不同：本夹具解析客户端发来的
// TEXT 帧（客户端帧带掩码），校验 Mistral Realtime 客户端事件契约：
//   * session.update 的 session.audio_format 必须精确为 pcm_s16le / 16000Hz；
//   * input_audio.append 先于 input_audio.flush 先于 input_audio.end；
// 并在脚本化时机回发服务端事件（session.created / delta / segment / done /
// error）或直接关闭连接。帧编码与握手校验复用 fake_ws_server.h，不重复实现。
//
// 有界与并发安全：收发用 SO_RCVTIMEO/SO_SNDTIMEO，循环轮询 stopping_，任何
// 阻塞调用都不会无限等待；Stop() 通过 shutdown(listenFd) 打断 accept()，服务
// 线程自行 close() 其接受的连接，Stop() 从不触碰该 fd，避免 fd 复用竞态。
// 观测字段用原子量/互斥量，可在 worker 运行时安全轮询。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <json/json.h>

#include "fake_ws_server.h"

namespace mistral_fixture {

using namespace std::chrono_literals;

enum class Scenario {
    HttpError,             // 握手上游回 HTTP <status>（非 101），或悬挂不响应
    Normal,                // created → update → end 后回 delta/segment/delta/done
    DelayedCreatedNormal,  // 握手后延迟 created（End 早于 created 仍须完成）
    ServerError,           // created → update 后回 errorJson（嵌套 error.code/message）
    InvalidEvent,          // created → update 后回非法 JSON 载荷
    CloseAfterDelta,       // created → end 后回一条 delta，随即关闭连接
    Passive,               // created（可关）之后保持沉默
};

class MistralWsServer {
public:
    explicit MistralWsServer(Scenario scenario) : scenario_(scenario) {}
    ~MistralWsServer() { Stop(); }

    MistralWsServer(const MistralWsServer&) = delete;
    MistralWsServer& operator=(const MistralWsServer&) = delete;

    void SetHttpStatus(long status) { httpStatus_ = status; }
    /// 前 hangAfter 条连接回 httpStatus，之后悬挂不响应（-1 = 永不悬挂）。
    void SetHangAfter(long hangAfter) { hangAfter_ = hangAfter; }
    void SetCreatedDelay(std::chrono::milliseconds delay) { createdDelay_ = delay; }
    void SetSendCreated(bool value) { sendCreated_ = value; }
    void SetErrorJson(std::string json) { errorJson_ = std::move(json); }
    void SetInvalidJson(std::string json) { invalidJson_ = std::move(json); }
    void SetCloseDeltaText(std::string text) { closeDeltaText_ = std::move(text); }

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
        if (::listen(listenFd_, 8) != 0) return false;
        thread_ = std::thread([this] { Serve(); });
        return true;
    }

    void Stop() {
        stopping_.store(true);
        if (listenFd_ >= 0) ::shutdown(listenFd_, SHUT_RDWR);  // 打断 accept()
        if (thread_.joinable()) thread_.join();
        if (listenFd_ >= 0) ::close(listenFd_);
        listenFd_ = -1;
    }

    std::string Url() const {
        return "ws://127.0.0.1:" + std::to_string(port_) + "/";
    }

    int AcceptCount() const { return accepts_.load(); }
    bool HandshakeSent() const { return handshakeSent_.load(); }
    long LastStatusSent() const { return lastStatusSent_.load(); }
    bool UpdateSeen() const { return updateSeq_.load() > 0; }
    bool UpdateAudioFormatOk() const { return updateFormatOk_.load(); }
    bool CreatedBeforeUpdate() const {
        const int created = createdSeq_.load();
        const int update = updateSeq_.load();
        return created > 0 && update > created && !clientSentBeforeCreated_.load();
    }
    bool AppendSeen() const { return appendSeq_.load() > 0; }
    bool FlushSeen() const { return flushSeq_.load() > 0; }
    bool EndSeen() const { return endSeq_.load() > 0; }
    bool FlushBeforeEnd() const { return flushBeforeEnd_.load(); }
    std::string RequestPath() const {
        std::lock_guard<std::mutex> lock(stringsMutex_);
        return path_;
    }
    std::string AuthHeader() const {
        std::lock_guard<std::mutex> lock(stringsMutex_);
        return auth_;
    }

private:
    bool ReadRequest(int fd, std::string& request) {
        std::array<char, 4096> buf{};
        while (!stopping_.load() && request.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
            if (n > 0) { request.append(buf.data(), size_t(n)); continue; }
            if (n == 0) return false;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        return request.find("\r\n\r\n") != std::string::npos;
    }

    bool SendAll(int fd, const uint8_t* data, size_t size) {
        size_t off = 0;
        while (off < size) {
            if (stopping_.load()) return false;
            const ssize_t n = ::send(fd, data + off, size - off, MSG_NOSIGNAL);
            if (n > 0) { off += size_t(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            return false;
        }
        return true;
    }

    bool SendRaw(int fd, const std::string& data) {
        return SendAll(fd, reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    bool SendText(int fd, const std::string& payload) {
        const auto frame = fcitxws::EncodeTextFrame(payload);
        return SendAll(fd, frame.data(), frame.size());
    }

    void SleepInterruptible(std::chrono::milliseconds total) {
        const auto deadline = std::chrono::steady_clock::now() + total;
        while (!stopping_.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
    }

    static std::string HttpErrorResponse(long status) {
        return "HTTP/1.1 " + std::to_string(status) + " Error\r\n"
               "Content-Length: 0\r\nConnection: close\r\n\r\n";
    }

    bool SendHandshake(int fd, const std::string& request) {
        std::string key;
        const size_t pos = request.find("Sec-WebSocket-Key:");
        if (pos != std::string::npos) {
            size_t start = pos + std::strlen("Sec-WebSocket-Key:");
            while (start < request.size() && request[start] == ' ') ++start;
            const size_t end = request.find("\r\n", start);
            key = request.substr(start, end - start);
        }
        fcitxws::Sha1 sha;
        const auto digest =
            sha.Digest(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        const std::string accept = fcitxws::Base64(digest.data(), digest.size());
        return SendRaw(fd, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                           "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept +
                           "\r\n\r\n");
    }

    static bool TryPopFrame(std::vector<uint8_t>& buf, uint8_t& opcode,
                            std::string& payload) {
        if (buf.size() < 2) return false;
        const uint8_t b0 = buf[0];
        const uint8_t b1 = buf[1];
        opcode = b0 & 0x0f;
        const bool masked = (b1 & 0x80) != 0;
        uint64_t length = b1 & 0x7f;
        size_t header = 2;
        if (length == 126) {
            if (buf.size() < 4) return false;
            length = (uint64_t(buf[2]) << 8) | buf[3];
            header = 4;
        } else if (length == 127) {
            if (buf.size() < 10) return false;
            length = 0;
            for (int i = 0; i < 8; ++i) length = (length << 8) | buf[2 + i];
            header = 10;
        }
        const size_t maskLen = masked ? 4 : 0;
        if (length > 1024 * 1024 || buf.size() < header + maskLen + length) return false;
        payload.assign(
            reinterpret_cast<const char*>(buf.data() + header + maskLen),
            size_t(length));
        if (masked) {
            const uint8_t* mask = buf.data() + header;
            for (size_t i = 0; i < payload.size(); ++i)
                payload[i] = char(uint8_t(payload[i]) ^ mask[i % 4]);
        }
        buf.erase(buf.begin(), buf.begin() + header + maskLen + length);
        return true;
    }

    static std::string Serialize(const Json::Value& value) {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        return Json::writeString(builder, value);
    }

    static bool ValidateAudioFormat(const Json::Value& json) {
        if (!json.isMember("session") || !json["session"].isObject()) return false;
        const auto& session = json["session"];
        if (!session.isMember("audio_format") || !session["audio_format"].isObject())
            return false;
        const auto& format = session["audio_format"];
        if (!format.isMember("encoding") || !format["encoding"].isString()) return false;
        if (format["encoding"].asString() != "pcm_s16le") return false;
        if (!format.isMember("sample_rate") || !format["sample_rate"].isInt()) return false;
        return format["sample_rate"].asInt() == 16000;
    }

    bool OnEnd(int fd) {
        if (scenario_ == Scenario::Normal ||
            scenario_ == Scenario::DelayedCreatedNormal) {
            SendText(fd, R"({"type":"transcription.text.delta","text":"hello"})");
            SendText(fd, R"({"type":"transcription.segment","text":"hello"})");
            SendText(fd, R"({"type":"transcription.text.delta","text":"world"})");
            SendText(fd, R"({"type":"transcription.done","text":"hello world"})");
            return true;
        }
        if (scenario_ == Scenario::CloseAfterDelta) {
            Json::Value event;
            event["type"] = "transcription.text.delta";
            event["text"] = closeDeltaText_;
            SendText(fd, Serialize(event));
            SleepInterruptible(120ms);
            ::shutdown(fd, SHUT_RDWR);  // 模拟服务端在终态前直接断链
            return false;
        }
        return true;
    }

    bool OnClientMessage(int fd, const std::string& payload) {
        Json::Value json;
        Json::Reader reader;
        if (!reader.parse(payload, json) || !json.isObject() ||
            !json["type"].isString())
            return true;
        const std::string type = json["type"].asString();
        if (type == "session.update") {
            updateSeq_.store(++sequence_);
            updateFormatOk_.store(ValidateAudioFormat(json));
            if (!errorJson_.empty()) { SendText(fd, errorJson_); return true; }
            if (!invalidJson_.empty()) { SendText(fd, invalidJson_); return true; }
        } else if (type == "input_audio.append") {
            appendSeq_.store(++sequence_);
        } else if (type == "input_audio.flush") {
            flushSeq_.store(++sequence_);
        } else if (type == "input_audio.end") {
            const int endSeq = ++sequence_;
            endSeq_.store(endSeq);
            flushBeforeEnd_.store(appendSeq_.load() > 0 &&
                                  flushSeq_.load() > appendSeq_.load() &&
                                  flushSeq_.load() < endSeq);
            return OnEnd(fd);
        }
        return true;
    }

    void HandleConnection(int fd) {
        timeval rcv{0, 100000};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
        timeval snd{0, 100000};
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));

        std::string request;
        if (!ReadRequest(fd, request)) return;

        {
            std::lock_guard<std::mutex> lock(stringsMutex_);
            const size_t firstSpace = request.find(' ');
            const size_t secondSpace = request.find(' ', firstSpace + 1);
            if (firstSpace != std::string::npos && secondSpace != std::string::npos)
                path_ = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);
            const size_t authPos = request.find("Authorization:");
            if (authPos != std::string::npos) {
                size_t start = authPos + std::strlen("Authorization:");
                while (start < request.size() && request[start] == ' ') ++start;
                const size_t end = request.find("\r\n", start);
                auth_ = request.substr(start, end - start);
            }
        }

        if (scenario_ == Scenario::HttpError) {
            const bool hang = httpStatus_ == 0 ||
                              (hangAfter_ >= 0 && accepts_.load() > hangAfter_);
            if (hang) {
                // 悬挂：不响应 HTTP 握手，直到 Stop() 打断（模拟上游无响应）。
                while (!stopping_.load()) std::this_thread::sleep_for(20ms);
                return;
            }
            SendRaw(fd, HttpErrorResponse(httpStatus_));
            lastStatusSent_.store(httpStatus_);
            return;
        }

        if (!SendHandshake(fd, request)) return;
        handshakeSent_.store(true);

        if (sendCreated_) {
            if (createdDelay_.count() > 0) SleepInterruptible(createdDelay_);
            // Sequence numbers at processing time alone cannot catch a client
            // that sent update while created was delayed: inspect buffered IO.
            uint8_t earlyByte;
            clientSentBeforeCreated_.store(
                ::recv(fd, &earlyByte, 1, MSG_PEEK | MSG_DONTWAIT) > 0);
            SendText(fd, R"({"type":"session.created"})");
            createdSeq_.store(++sequence_);
        }

        std::vector<uint8_t> buffer;
        std::array<uint8_t, 4096> chunk{};
        while (!stopping_.load()) {
            uint8_t opcode = 0;
            std::string payload;
            while (TryPopFrame(buffer, opcode, payload)) {
                if (opcode == 0x8) return;          // 客户端 close
                if (opcode != 0x1) continue;        // 只看 TEXT 帧
                if (!OnClientMessage(fd, payload)) return;
            }
            const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
            if (n > 0) {
                buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + n);
                if (buffer.size() > 1024 * 1024 + 14) return;
                continue;
            }
            if (n == 0) return;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return;
        }
    }

    void Serve() {
        const int lfd = listenFd_;  // 服务线程内捕获；Stop 只 shutdown 不 close
        while (!stopping_.load()) {
            const int fd = ::accept(lfd, nullptr, nullptr);
            if (fd < 0) {
                if (stopping_.load()) break;
                if (errno == EINTR) continue;
                break;
            }
            accepts_.fetch_add(1);
            HandleConnection(fd);
            ::close(fd);
            // 仅 HTTP 失败场景保留监听以观测是否发生重试连接。
            if (scenario_ != Scenario::HttpError) break;
        }
    }

    Scenario scenario_;
    long httpStatus_ = 0;
    long hangAfter_ = -1;
    std::chrono::milliseconds createdDelay_{0};
    bool sendCreated_ = true;
    std::string errorJson_;
    std::string invalidJson_;
    std::string closeDeltaText_ = "hello";

    int listenFd_ = -1;  // 仅 Start/Stop（同一线程）读写
    uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<int> accepts_{0};
    std::atomic<bool> handshakeSent_{false};
    std::atomic<long> lastStatusSent_{0};
    std::thread thread_;

    int sequence_ = 0;  // 仅服务线程写
    std::atomic<int> createdSeq_{0};
    std::atomic<int> updateSeq_{0};
    std::atomic<int> appendSeq_{0};
    std::atomic<int> flushSeq_{0};
    std::atomic<int> endSeq_{0};
    std::atomic<bool> updateFormatOk_{false};
    std::atomic<bool> flushBeforeEnd_{false};
    std::atomic<bool> clientSentBeforeCreated_{false};

    mutable std::mutex stringsMutex_;
    std::string path_;
    std::string auth_;
};

} // namespace mistral_fixture
