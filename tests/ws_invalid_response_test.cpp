// 非法服务端消息不得崩溃宿主进程（AsrSession 健壮性回归）。
//
// 背景：jsoncpp 的 asString()/asInt()/asBool() 在字段类型不符时抛
// Json::LogicError。流式会话的 worker 线程没有任何 try/catch，异常逃逸线程
// 函数即 std::terminate → 整个 fcitx5 宿主 abort。真实云服务在异常/回归/
// 中间人场景下可能下发类型错误的已识别字段（如 delta 为数组、code 为对象），
// 因此必须把「结构非法的已识别事件」当成协议错误就地终止会话，而不是崩溃。
//
// 本测试用假 WS 服务端向真实 AsrSession 注入单条畸形消息，断言：
//   * 恰好一次固定协议错误（"Invalid Realtime event" / "Invalid Volcengine response"）；
//   * 恰好一个空 final，且不产生任何 partial/bogus 文本；
//   * 会话在 2s 内自行结束（不调用 End()/Cancel()），JoinWithTimeout 立即回收；
//   * final 携带正确 sessionId；
//   * 可选字段为 null（error.code）不应被误判为协议错误（容错用例）。
//
// 依赖真实本地 WS（libcurl 的 ws 支持）；平台缺失时返回 77，由 CTest 记为 SKIP。

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <curl/curl.h>

#include "asr/realtime_asr.h"
#include "asr/volcengine_asr.h"
#include "support/fake_ws_server.h"

namespace {

using namespace std::chrono_literals;

enum class Backend { Realtime, Volcengine };

bool Check(bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool LibcurlSupportsWebSocket() {
    curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (!info || !info->protocols) return false;
    for (const char* const* p = info->protocols; *p; ++p) {
        if (std::strcmp(*p, "ws") == 0) return true;
    }
    return false;
}

struct Case {
    std::string name;
    std::string json;
    bool expectInvalid = true;  // false = 容错用例（不得判为协议错误）
    bool gzipFrame = false;     // volcengine：gzip 标记，但载荷不是合法 gzip
};

/// result 数组深度超过提取器上限（>16）的嵌套结构。
std::string MakeDeepResultJson(int depth) {
    std::string s = "{\"code\":20000000,\"result\":";
    s.append(size_t(depth), '[');
    s += "{\"text\":\"x\"}";
    s.append(size_t(depth), ']');
    s += "}";
    return s;
}

std::string ExpectedError(Backend backend) {
    return backend == Backend::Realtime ? "Invalid Realtime event"
                                         : "Invalid Volcengine response";
}

std::vector<uint8_t> BuildFrame(const Case& c, Backend backend) {
    if (backend == Backend::Realtime) return fcitxws::EncodeTextFrame(c.json);
    if (c.gzipFrame) {
        const std::vector<uint8_t> payload(c.json.begin(), c.json.end());
        return fcitxws::EncodeBinaryFrame(
            fcitxws::VolcengineFrame(0x9, 0x3, 0x1, 0x1, 1, payload));
    }
    return fcitxws::EncodeBinaryFrame(
        fcitxws::VolcengineResponseFrame(c.json, 1, 0x3));
}

struct Outcome {
    std::atomic<int> errorCount{0};
    std::atomic<int> finalCount{0};
    std::atomic<int> partialCount{0};
    std::atomic<uint64_t> finalSessionId{0};
    std::mutex mutex;
    std::string errorText;
    std::string finalText;
    std::string bogusText;
};

void RecordError(Outcome& out, const std::string& error) {
    out.errorCount.fetch_add(1);
    std::lock_guard<std::mutex> lock(out.mutex);
    if (out.errorText.empty()) out.errorText = error;
}

void RecordResult(Outcome& out, const std::string& text, bool isFinal,
                  uint64_t sid) {
    if (isFinal) {
        out.finalCount.fetch_add(1);
        out.finalSessionId.store(sid);
        std::lock_guard<std::mutex> lock(out.mutex);
        out.finalText = text;
        return;
    }
    out.partialCount.fetch_add(1);
    std::lock_guard<std::mutex> lock(out.mutex);
    if (out.bogusText.empty()) out.bogusText = text;
}

/// 每条畸形消息都不得中止进程、不得挂起：要么在 2s 内以固定协议错误结束，
/// 要么（容错用例）保持运行且不报协议错误。
bool RunCase(const Case& c, Backend backend) {
    const std::string expected = ExpectedError(backend);
    auto frame = BuildFrame(c, backend);
    if (!c.expectInvalid) {
        auto control = fcitxws::EncodeTextFrame(
            R"({"type":"conversation.item.input_audio_transcription.delta","delta":"control"})");
        frame.insert(frame.end(), control.begin(), control.end());
    }
    fcitxws::FakeWsServer server(frame, frame.size(), 0ms, 150ms);
    if (!Check(server.Start(), c.name + ": fake WS server must listen"))
        return false;

    auto outcome = std::make_shared<Outcome>();
    auto& out = *outcome;
    const uint64_t sid = 1;
    fcitx::AsrEngine::Config config;
    config.apiEndpoint = server.Url();
    config.apiKey = "test-key";
    config.modelName =
        backend == Backend::Realtime ? "gpt-live-transcribe" : "bigmodel";

    std::shared_ptr<fcitx::AsrSession> session;
    auto errorCb = [outcome](const std::string& e) { RecordError(*outcome, e); };
    if (backend == Backend::Realtime) {
        session = std::make_shared<fcitx::RealtimeAsrSession>(config, errorCb, sid);
    } else {
        session =
            std::make_shared<fcitx::VolcengineAsrSession>(config, errorCb, sid);
    }
    session->SetResultCallback(
        [outcome](const std::string& t, bool f, uint64_t s) {
            RecordResult(*outcome, t, f, s);
        });
    session->StartWorker();

    if (!c.expectInvalid) {
        // 容错用例：可选字段 null 不构成结构错误——会话不得终止，也不得报协议错误。
        // Observing a following valid delta proves the nullable event was
        // consumed; a stalled handshake must not look like successful parsing.
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (out.partialCount.load() == 0 &&
               !session->GetState()->workerDone.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        const bool terminated = session->GetState()->workerDone.load();
        std::string err, observed;
        {
            std::lock_guard<std::mutex> lock(out.mutex);
            err = out.errorText;
            observed = out.bogusText;
        }
        session->Cancel();
        session->JoinWithTimeout(2s);
        server.Stop();
        return Check(!terminated,
                      c.name + ": an optional null field must not terminate the session") &&
               Check(out.partialCount.load() == 1 && observed == "control",
                      c.name + ": the following valid delta must be delivered") &&
               Check(out.finalCount.load() == 0,
                      c.name + ": optional fields must not publish a premature final") &&
               Check(err != expected,
                     c.name + ": an optional null field must not be a protocol error");
    }

    // 无效用例：必须在 2s 内自行结束（不得先用 End()/Cancel() 触发）。
    const bool workerDone =
        fcitx::WaitForWorkerCompletion(session->GetState()->workerDone, 2s);
    const bool finished = session->GetState()->finished.load();

    if (!workerDone) {
        session->Cancel();  // 清理，避免失败用例泄漏 worker 线程
        session->JoinWithTimeout(2s);
        server.Stop();
        return Check(false,
                     c.name + ": a malformed server message must terminate the worker "
                              "within 2s without End()/Cancel()");
    }

    const auto joinStart = std::chrono::steady_clock::now();
    session->JoinWithTimeout(500ms);
    const auto joinMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - joinStart).count();
    server.Stop();

    std::string err, fin, bogus;
    {
        std::lock_guard<std::mutex> lock(out.mutex);
        err = out.errorText;
        fin = out.finalText;
        bogus = out.bogusText;
    }
    bool ok = true;
    ok = Check(out.errorCount.load() == 1,
               c.name + ": exactly one protocol error expected") && ok;
    ok = Check(err == expected,
               c.name + ": protocol error must be \"" + expected + "\"") && ok;
    ok = Check(out.finalCount.load() == 1,
               c.name + ": exactly one final expected") && ok;
    ok = Check(fin.empty(), c.name + ": the final text must be empty") && ok;
    ok = Check(out.partialCount.load() == 0,
               c.name + ": no partial text expected") && ok;
    ok = Check(bogus.empty(), c.name + ": no bogus text expected") && ok;
    ok = Check(finished, c.name + ": session must be finished") && ok;
    ok = Check(out.finalSessionId.load() == sid,
               c.name + ": final must carry the session id") && ok;
    ok = Check(joinMs <= 500,
               c.name + ": JoinWithTimeout must reclaim the finished worker") && ok;
    return ok;
}

const std::vector<Case> kRealtimeCases = {
    // 首要复现：delta 为数组 → 基线 asString() 抛 Json::LogicError → 宿主 abort。
    // 注意：官方类型中 delta 为 Optional[str]=None，故 delta 缺失/null 是合法对照
    // （见文末 valid_* 用例），不属于畸形矩阵。
    {"primary_delta_array",
     R"({"type":"conversation.item.input_audio_transcription.delta","delta":[]})"},
    {"delta_object",
     R"({"type":"conversation.item.input_audio_transcription.delta","delta":{}})"},
    // completed.transcript 为必填 str：null/数组均为畸形。
    {"transcript_array",
     R"({"type":"conversation.item.input_audio_transcription.completed","transcript":[]})"},
    {"transcript_null",
     R"({"type":"conversation.item.input_audio_transcription.completed","transcript":null})"},
    // item_id 为必填 str（仅“缺失”保留 legacy 空值）：存在但类型错/null 均畸形。
    {"item_id_object",
     R"({"type":"conversation.item.input_audio_transcription.completed","transcript":"x","item_id":{}})"},
    {"item_id_null",
     R"({"type":"conversation.item.input_audio_transcription.completed","transcript":"x","item_id":null})"},
    // error 字段存在但非对象（数组/null）畸形；其可空子字段的 null 见 valid_*。
    {"error_object_array", R"({"type":"error","error":[]})"},
    {"error_null", R"({"type":"error","error":null})"},
    {"error_message_array", R"({"type":"error","error":{"message":[]}})"},
    {"syntax_invalid", "{\"type\":"},
    {"root_null", "null"},
    {"root_array", "[]"},
    {"root_scalar", "12345"},
    // 合法对照：delta 可选，null 不得被判为协议错误，会话不得终止。
    {"valid_delta_null",
     R"({"type":"conversation.item.input_audio_transcription.delta","delta":null})",
     false},
    // 合法对照：error.message/code/type 皆 Optional[str]=None，null/缺失不得误判。
    {"valid_error_nullable_fields",
     R"({"type":"error","error":{"message":"boom","code":null,"type":null}})",
     false},
    {"valid_error_message_null",
     R"({"type":"error","error":{"message":null}})", false},
    {"valid_error_empty_object", R"({"type":"error","error":{}})", false},
};

const std::vector<Case> kVolcengineCases = {
    // 首要复现：code 为对象 → 基线 asInt() 抛 Json::LogicError → 宿主 abort。
    {"primary_code_object", R"({"code":{},"result":{"text":"fixture"}})"},
    {"code_array", R"({"code":[],"result":{"text":"fixture"}})"},
    {"code_string", R"({"code":"x","result":{"text":"fixture"}})"},
    {"code_null", R"({"code":null,"result":{"text":"fixture"}})"},
    {"code_int_overflow",
     R"({"code":99999999999999999999,"result":{"text":"fixture"}})"},
    {"result_scalar", R"({"code":20000000,"result":"fixture"})"},
    {"result_text_array", R"({"code":20000000,"result":{"text":[]}})"},
    {"result_utterances_object",
     R"({"code":20000000,"result":{"utterances":{}}})"},
    {"utterance_text_array",
     R"({"code":20000000,"result":{"utterances":[{"text":[],"definite":true}]}})"},
    {"utterance_definite_array",
     R"({"code":20000000,"result":{"utterances":[{"text":"x","definite":[]}]}})"},
    {"syntax_invalid", "{\"code\":"},
    {"root_null", "null"},
    {"deep_result_array", MakeDeepResultJson(20)},
    {"invalid_gzip_frame", "not-a-gzip-stream", true, true},
};

} // namespace

int main(int argc, char** argv) {
    // 假服务端只在 127.0.0.1 监听，必须绕过任何用户代理配置。
    if (setenv("NO_PROXY", "127.0.0.1", 1) != 0 ||
        setenv("no_proxy", "127.0.0.1", 1) != 0) return 2;

    if (argc != 2 || (std::string(argv[1]) != "realtime" &&
                      std::string(argv[1]) != "volcengine")) {
        std::cerr << "usage: ws_invalid_response_test <realtime|volcengine>\n";
        return 2;
    }
    if (!LibcurlSupportsWebSocket()) {
        std::cerr << "SKIP: libcurl has no WebSocket support on this platform; "
                     "streaming backends are unavailable here.\n";
        return 77;  // CTest SKIP_RETURN_CODE
    }

    const auto backend = (std::string(argv[1]) == "realtime") ? Backend::Realtime
                                                             : Backend::Volcengine;
    const std::vector<Case>& cases =
        backend == Backend::Realtime ? kRealtimeCases : kVolcengineCases;

    bool ok = true;
    for (const Case& c : cases) {
        const bool pass = RunCase(c, backend);
        std::cerr << "  [" << (pass ? "PASS" : "FAIL") << "] " << c.name << '\n';
        ok = pass && ok;
        if (!pass) break;  // 首个失败即可定位，避免级联耗时
    }
    return ok ? 0 : 1;
}
