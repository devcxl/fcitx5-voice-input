// Mistral Realtime 会话端到端回归（CRASH-ASR-001）。
//
// 交互式本地假服务端（support/mistral_ws_server.h，仅 127.0.0.1，无真实凭据）
// 驱动真实 MistralAsrSession：握手 401/404 一次净化错误+空 final 且不重连；悬挂
// 握手期间 End 静默收尾零传输错误；瞬时 503 后重试期间 End 保留 503；正常路径
// created→update(16k PCM)→append/flush/end 顺序与不重复终态；嵌套 error 数值码不
// 回显不可信文本；畸形事件 → 一次 "Invalid Mistral event" + 空 final；End 后先
// delta 再断链的累积终态；回调抛异常不 abort；Cancel 无 final；created 缺失有界；
// End 早于延迟 created 仍送出排队音频（End != Cancel）。缺 ws 支持返回 77（SKIP）。

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include "asr/mistral_asr.h"
#include "support/mistral_ws_server.h"
#include "support/mistral_session_observer.h"

namespace {

using namespace std::chrono_literals;
using namespace mistral_fixture;

bool Check(bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool SupportsWebSocket() {
    const auto* info = curl_version_info(CURLVERSION_NOW);
    if (!info || !info->protocols) return false;
    for (const char* const* p = info->protocols; *p; ++p)
        if (std::strcmp(*p, "ws") == 0) return true;
    return false;
}

bool WaitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

using SessionPtr = std::shared_ptr<fcitx::MistralAsrSession>;

bool WaitDone(const SessionPtr& session, std::chrono::milliseconds timeout) {
    return fcitx::WaitForWorkerCompletion(session->GetState()->workerDone, timeout);
}

void FeedAudioFor(const SessionPtr& session, std::chrono::milliseconds duration) {
    std::vector<float> audio(1600, 0.01f);
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline &&
           !session->GetState()->workerDone.load()) {
        session->FeedAudio(audio.data(), audio.size());
        std::this_thread::sleep_for(5ms);
    }
}

void Finish(const SessionPtr& session, MistralWsServer& server, bool done) {
    if (!done) {
        session->Cancel();
        session->JoinWithTimeout(2s);
    } else {
        session->JoinWithTimeout(1s);
    }
    server.Stop();
}

long ElapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}
// ── 握手 HTTP 401/404：一次净化错误 + 空 final，不重连 ──
bool RunHttpError(const std::string& name, long status) {
    MistralWsServer server(Scenario::HttpError);
    server.SetHttpStatus(status);
    if (!Check(server.Start(), name + ": fake server must listen")) return false;
    auto outcome = std::make_shared<Outcome>();
    auto& out = *outcome;
    auto session = MakeSession(server.Url(), 0, outcome);
    const auto start = std::chrono::steady_clock::now();
    session->StartWorker();
    const bool done = WaitDone(session, 5s);
    const long ms = ElapsedMs(start);
    Finish(session, server, done);
    const std::string err = ErrorText(out);
    bool ok = true;
    ok = Check(done && ms < 2000,
               name + ": permanent HTTP failure must not retry (elapsed " +
                   std::to_string(ms) + "ms)") &&
         ok;
    ok = Check(out.errors.load() == 1, name + ": exactly one sanitized error") && ok;
    ok = Check(err.find("HTTP " + std::to_string(status)) != std::string::npos,
               name + ": error must name the HTTP status") &&
         ok;
    ok = Check(err.find(kApiKey) == std::string::npos,
               name + ": error must not leak credentials") &&
         ok;
    ok = Check(out.finals.load() == 1 && FinalText(out).empty(),
               name + ": exactly one empty final") &&
         ok;
    ok = Check(out.finalSessionId.load() == kSessionId,
               name + ": final must carry the session id") &&
         ok;
    ok = Check(server.AcceptCount() == 1,
               name + ": no retry must open no second connection") &&
         ok;
    return ok;
}
// ── 正常路径与部分/已完成回调异常（走同一 Normal 流程）──
bool RunNormalLike(const std::string& name, int throwMode) {
    MistralWsServer server(Scenario::Normal);
    if (!Check(server.Start(), name + ": fake server must listen")) return false;
    auto outcome = std::make_shared<Outcome>();
    auto& out = *outcome;
    auto session = MakeSession(server.Url(), throwMode, outcome);
    session->StartWorker();
    bool ok = true;
    ok = Check(WaitFor([&] { return server.UpdateSeen(); }, 3s),
               name + ": client must send session.update after session.created") &&
         ok;
    FeedAudioFor(session, 1200ms);  // append + 周期 flush 在 End 之前
    ok = Check(server.AppendSeen(), name + ": input_audio.append expected") && ok;
    session->End();
    const bool done = WaitDone(session, 5s);
    Finish(session, server, done);
    const std::string err = ErrorText(out);
    const std::string finalText = FinalText(out);
    ok = Check(done, name + ": worker must reach its terminal state") && ok;
    ok = Check(server.UpdateAudioFormatOk(),
               name + ": audio_format must be exactly 16kHz pcm_s16le") &&
         ok;
    ok = Check(server.CreatedBeforeUpdate(), name + ": created must precede update") && ok;
    ok = Check(server.FlushSeen() && server.FlushBeforeEnd(),
               name + ": flush must be sent before input_audio.end") &&
         ok;
    ok = Check(server.EndSeen(), name + ": input_audio.end expected") && ok;
    ok = Check(server.RequestPath().find("audio/transcriptions/realtime?model=") !=
                   std::string::npos,
               name + ": endpoint path must target realtime transcriptions") &&
         ok;
    ok = Check(server.AuthHeader() == std::string("Bearer ") + kApiKey,
               name + ": Authorization header must carry the configured key") &&
         ok;
    ok = Check(out.finals.load() == 1, name + ": exactly one final") && ok;
    if (throwMode == 0) {
        ok = Check(finalText == "hello world",
                   name + ": final must be exactly \"hello world\"") &&
             ok;
        ok = Check(out.errors.load() == 0, name + ": normal path must not error") && ok;
    } else if (throwMode == 1) {
        ok = Check(finalText.empty(), name + ": boundary fallback final must be empty") && ok;
        ok = Check(out.errors.load() == 1 && err == "ASR worker exception",
                   name + ": partial callback throw must yield one boundary error") &&
             ok;
    } else {
        ok = Check(finalText == "hello world",
                   name + ": the thrown final must still be observed once") &&
             ok;
        ok = Check(out.errors.load() == 1 && err == "ASR worker exception",
                   name + ": final callback throw must yield one boundary error") &&
             ok;
    }
    return ok;
}
// ── 服务端 error 事件（嵌套 code/message）与 error 回调异常 ──
bool RunServerError(const std::string& name, const std::string& json,
                    const std::string& codeMarker, int throwMode) {
    MistralWsServer server(Scenario::ServerError);
    server.SetErrorJson(json);
    if (!Check(server.Start(), name + ": fake server must listen")) return false;
    auto outcome = std::make_shared<Outcome>();
    auto& out = *outcome;
    auto session = MakeSession(server.Url(), throwMode, outcome);
    const auto start = std::chrono::steady_clock::now();
    session->StartWorker();
    const bool done = WaitDone(session, 3s);
    const long ms = ElapsedMs(start);
    Finish(session, server, done);
    const std::string err = ErrorText(out);
    bool ok = true;
    ok = Check(done, name + ": server error must terminate promptly (" +
                          std::to_string(ms) + "ms)") &&
         ok;
    ok = Check(out.errors.load() == 1, name + ": exactly one error") && ok;
    ok = Check(err.find(codeMarker) != std::string::npos,
               name + ": safe error must include numeric code " + codeMarker) &&
         ok;
    ok = Check(err.find("invalid request") == std::string::npos,
               name + ": must not reflect untrusted error.message text") &&
         ok;
    ok = Check(out.finals.load() == 1 && FinalText(out).empty(),
               name + ": exactly one empty final") &&
         ok;
    return ok;
}
// ── 畸形服务端事件：一次 "Invalid Mistral event" + 空 final，不崩溃 ──
bool RunInvalid(const std::string& name, const std::string& json) {
    MistralWsServer server(Scenario::InvalidEvent);
    server.SetInvalidJson(json);
    if (!Check(server.Start(), name + ": fake server must listen")) return false;
    auto outcome = std::make_shared<Outcome>();
    auto& out = *outcome;
    auto session = MakeSession(server.Url(), 0, outcome);
    session->StartWorker();
    const bool done = WaitDone(session, 2s);
    Finish(session, server, done);
    const std::string err = ErrorText(out);
    bool ok = true;
    ok = Check(done, name + ": malformed event must self-terminate within 2s") && ok;
    ok = Check(out.errors.load() == 1, name + ": exactly one protocol error") && ok;
    ok = Check(err == "Invalid Mistral event",
               name + ": error must be \"Invalid Mistral event\"") &&
         ok;
    ok = Check(out.finals.load() == 1 && FinalText(out).empty(),
               name + ": exactly one empty final") &&
         ok;
    ok = Check(out.partials.load() == 0, name + ": no partial expected") && ok;
    return ok;
}
// ── End 后服务端先发 delta 再断链：累积文本作为 final ──
bool RunCloseAfterDelta(const std::string& name) {
    MistralWsServer server(Scenario::CloseAfterDelta);
    server.SetCloseDeltaText("hello");
    if (!Check(server.Start(), name + ": fake server must listen")) return false;
    auto outcome = std::make_shared<Outcome>();
    auto& out = *outcome;
    auto session = MakeSession(server.Url(), 0, outcome);
    session->StartWorker();
    bool ok = true;
    ok = Check(WaitFor([&] { return server.UpdateSeen(); }, 3s),
               name + ": client must send session.update") &&
         ok;
    FeedAudioFor(session, 120ms);
    session->End();
    const bool done = WaitDone(session, 5s);
    Finish(session, server, done);
    ok = Check(done, name + ": worker must finish after connection close") && ok;
    ok = Check(out.finals.load() == 1, name + ": exactly one final") && ok;
    ok = Check(FinalText(out) == "hello",
               name + ": final must accumulate the delta received before close") &&
         ok;
    return ok;
}
// ── Cancel：不得上报 final ──
bool RunCancel(const std::string& name) {
    MistralWsServer server(Scenario::Passive);
    if (!Check(server.Start(), name + ": fake server must listen")) return false;
    auto outcome = std::make_shared<Outcome>();
    auto& out = *outcome;
    auto session = MakeSession(server.Url(), 0, outcome);
    session->StartWorker();
    bool ok = true;
    ok = Check(WaitFor([&] { return server.UpdateSeen(); }, 3s),
               name + ": client must reach the streaming loop") &&
         ok;
    FeedAudioFor(session, 100ms);
    session->Cancel();
    const bool done = WaitDone(session, 2s);
    Finish(session, server, done);
    ok = Check(done, name + ": cancelled worker must finish") && ok;
    ok = Check(out.finals.load() == 0, name + ": cancel must not publish a final") && ok;
    return ok;
}
// ── session.created 缺失：必须有界结束（约 10s），不得永久挂起 ──
bool RunNoCreated(const std::string& name) {
    MistralWsServer server(Scenario::Passive);
    server.SetSendCreated(false);
    if (!Check(server.Start(), name + ": fake server must listen")) return false;
    auto outcome = std::make_shared<Outcome>();
    auto& out = *outcome;
    auto session = MakeSession(server.Url(), 0, outcome);
    const auto start = std::chrono::steady_clock::now();
    session->StartWorker();
    const bool done = WaitDone(session, 12s);
    const long ms = ElapsedMs(start);
    Finish(session, server, done);
    bool ok = true;
    ok = Check(done, name + ": missing session.created must be bounded (elapsed " +
                          std::to_string(ms) + "ms)") &&
         ok;
    ok = Check(out.finals.load() == 1 && FinalText(out).empty(),
               name + ": bounded end must publish one empty final") &&
         ok;
    return ok;
}
// ── End 在首次 HTTP 握手悬挂期间：一次空 final，零传输错误 ──
bool RunEndDuringHandshake(const std::string& name) {
    MistralWsServer server(Scenario::HttpError);  // status 0 = 永不响应
    if (!Check(server.Start(), name + ": fake server must listen")) return false;
    auto outcome = std::make_shared<Outcome>();
    auto& out = *outcome;
    auto session = MakeSession(server.Url(), 0, outcome);
    session->StartWorker();
    bool ok = true;
    ok = Check(WaitFor([&] { return server.AcceptCount() >= 1; }, 2s),
               name + ": client must reach the handshake") &&
         ok;
    const auto start = std::chrono::steady_clock::now();
    session->End();
    const bool done = WaitDone(session, 3s);
    const long ms = ElapsedMs(start);
    Finish(session, server, done);
    const std::string err = ErrorText(out);
    ok = Check(done && ms < 3000,
               name + ": End abort must finish the worker (" + std::to_string(ms) +
                   "ms)") &&
         ok;
    ok = Check(out.finals.load() == 1 && FinalText(out).empty(),
               name + ": exactly one empty final") &&
         ok;
    ok = Check(out.errors.load() == 0,
               name + ": a clean End abort must report ZERO transport errors") &&
         ok;
    ok = Check(err.find("aborted") == std::string::npos,
               name + ": must never surface 'aborted by an application callback'") &&
         ok;
    return ok;
}
// ── 首次尝试真实瞬时 HTTP 503，重试期间 End：保留 HTTP 503，不可退化为中止 ──
bool RunEndDuringRetryTransient(const std::string& name) {
    MistralWsServer server(Scenario::HttpError);
    server.SetHttpStatus(503);  // 5xx：可重试
    server.SetHangAfter(1);     // 仅首连回 503，后续悬挂
    if (!Check(server.Start(), name + ": fake server must listen")) return false;
    auto outcome = std::make_shared<Outcome>();
    auto& out = *outcome;
    auto session = MakeSession(server.Url(), 0, outcome);
    session->StartWorker();
    bool ok = true;
    ok = Check(WaitFor([&] { return server.LastStatusSent() == 503; }, 3s),
               name + ": first attempt must receive HTTP 503") &&
         ok;
    ok = Check(WaitFor([&] { return server.AcceptCount() >= 2; }, 3s),
               name + ": retry must reach the second handshake") && ok;
    session->End();
    const bool done = WaitDone(session, 3s);
    Finish(session, server, done);
    const std::string err = ErrorText(out);
    ok = Check(done, name + ": End during retry must finish the worker") && ok;
    ok = Check(out.finals.load() == 1 && FinalText(out).empty(),
               name + ": exactly one empty final") &&
         ok;
    ok = Check(out.errors.load() == 1, name + ": exactly one preserved error") && ok;
    ok = Check(err.find("HTTP 503") != std::string::npos,
               name + ": must preserve the transient HTTP 503, not the abort") &&
         ok;
    ok = Check(err.find("aborted") == std::string::npos,
               name + ": must not replace HTTP 503 with an abort message") &&
         ok;
    return ok;
}
// ── End 早于（延迟到达的）session.created：非取消，须送出排队音频并完成 ──
bool RunEndBeforeCreatedDelayed(const std::string& name) {
    MistralWsServer server(Scenario::DelayedCreatedNormal);
    server.SetCreatedDelay(800ms);
    if (!Check(server.Start(), name + ": fake server must listen")) return false;
    auto outcome = std::make_shared<Outcome>();
    auto& out = *outcome;
    auto session = MakeSession(server.Url(), 0, outcome);
    session->StartWorker();
    bool ok = true;
    ok = Check(WaitFor([&] { return server.HandshakeSent(); }, 2s),
               name + ": handshake must complete first") &&
         ok;
    // 握手响应已送达、curl_easy_perform 已返回，此刻 End 发生在 created 等待中。
    std::this_thread::sleep_for(250ms);
    std::vector<float> audio(1600, 0.01f);
    session->FeedAudio(audio.data(), audio.size());
    session->FeedAudio(audio.data(), audio.size());
    session->End();
    const bool done = WaitDone(session, 5s);
    Finish(session, server, done);
    ok = Check(done, name + ": End before created must still finish (End != Cancel)") && ok;
    ok = Check(server.UpdateSeen(), name + ": update must be sent after created") && ok;
    ok = Check(server.CreatedBeforeUpdate(), name + ": client must await delayed created") && ok;
    ok = Check(server.AppendSeen(), name + ": queued audio must be flushed before end") && ok;
    ok = Check(server.FlushBeforeEnd(), name + ": flush must precede input_audio.end") && ok;
    ok = Check(server.EndSeen(), name + ": input_audio.end must be sent") && ok;
    ok = Check(out.finals.load() == 1 && FinalText(out) == "hello world",
               name + ": queued speech must still produce the final transcript") &&
         ok;
    return ok;
}

bool Dispatch(const std::string& name) {
    const std::string nestedError =
        R"({"type":"error","error":{"message":"invalid request","code":3501}})";
    const std::string dictError =
        R"({"type":"error","error":{"message":{"detail":"x"},"code":7}})";
    if (name == "http-401") return RunHttpError(name, 401);
    if (name == "http-404") return RunHttpError(name, 404);
    if (name == "normal") return RunNormalLike(name, 0);
    if (name == "nested-error") return RunServerError(name, nestedError, "3501", 0);
    if (name == "error-message-dict") return RunServerError(name, dictError, "7", 0);
    if (name == "invalid-text-array")
        return RunInvalid(name, R"({"type":"transcription.text.delta","text":[]})");
    if (name == "invalid-root") return RunInvalid(name, "[]");
    if (name == "invalid-message-array")
        return RunInvalid(name, R"({"type":"error","error":{"message":[]}})");
    if (name == "close-after-delta-end") return RunCloseAfterDelta(name);
    if (name == "callback-throw-partial") return RunNormalLike(name, 1);
    if (name == "callback-throw-final") return RunNormalLike(name, 2);
    if (name == "callback-throw-error") return RunServerError(name, nestedError, "3501", 3);
    if (name == "cancel") return RunCancel(name);
    if (name == "silent-no-created") return RunNoCreated(name);
    if (name == "end-during-handshake") return RunEndDuringHandshake(name);
    if (name == "end-during-retry-transient-503")
        return RunEndDuringRetryTransient(name);
    if (name == "end-before-created-delayed") return RunEndBeforeCreatedDelayed(name);
    std::cerr << "unknown scenario: " << name << '\n';
    return false;
}
} // namespace

int main(int argc, char** argv) {
    // 假服务端只在 127.0.0.1 监听，必须绕过任何用户代理配置。
    if (setenv("NO_PROXY", "127.0.0.1", 1) != 0 ||
        setenv("no_proxy", "127.0.0.1", 1) != 0)
        return 2;
    if (argc != 2) {
        std::cerr << "usage: mistral_session_test <scenario>\n";
        return 2;
    }
    if (!SupportsWebSocket()) {
        std::cerr << "SKIP: libcurl has no WebSocket support.\n";
        return 77;  // CTest SKIP_RETURN_CODE
    }
    const std::string name = argv[1];
    const bool passed = Dispatch(name);
    std::cerr << (passed ? "PASS: " : "FAIL: ") << name << '\n';
    return passed ? 0 : 1;
}
