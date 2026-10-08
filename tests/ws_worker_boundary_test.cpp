#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include "asr/realtime_asr.h"
#include "asr/volcengine_asr.h"
#include "support/fake_ws_server.h"

namespace {

using namespace std::chrono_literals;

bool SupportsWebSocket() {
    const auto* info = curl_version_info(CURLVERSION_NOW);
    if (!info || !info->protocols) return false;
    for (auto protocol = info->protocols; *protocol; ++protocol) {
        if (std::strcmp(*protocol, "ws") == 0) return true;
    }
    return false;
}

struct Outcome {
    std::atomic<int> partials{0};
    std::atomic<int> finals{0};
    std::atomic<int> errors{0};
    std::atomic<uint64_t> finalSessionId{0};
    std::string finalText; // read only after workerDone acquire + join
};

std::vector<uint8_t> Response(const std::string& backend, const std::string& mode,
                              size_t& writeSize) {
    std::vector<uint8_t> response;
    if (backend == "realtime") {
        if (mode == "error") {
            response = fcitxws::EncodeTextFrame(
                R"({"type":"error","error":{"message":"fixture"}})");
        }
        auto delta = fcitxws::EncodeTextFrame(
            R"({"type":"conversation.item.input_audio_transcription.delta","delta":"fixture"})");
        response.insert(response.end(), delta.begin(), delta.end());
        if (mode == "final") {
            // Separate the completed event so the caller can End() after the
            // first partial, without guessing how long the handshake takes.
            writeSize = delta.size();
            auto completed = fcitxws::EncodeTextFrame(
                R"({"type":"conversation.item.input_audio_transcription.completed","transcript":"fixture"})");
            response.insert(response.end(), completed.begin(), completed.end());
        }
    } else {
        if (mode == "empty-ack") {
            response = fcitxws::EncodeBinaryFrame(
                fcitxws::VolcengineResponseFrame("", 1, 0x1));
        }
        const std::string json = mode == "error"
            ? R"({"code":45000001,"message":"fixture"})"
            : R"({"code":20000000,"result":{"text":"fixture"}})";
        auto frame = fcitxws::EncodeBinaryFrame(
            fcitxws::VolcengineResponseFrame(json, 2));
        response.insert(response.end(), frame.begin(), frame.end());
    }
    return response;
}

bool WaitForPartial(const std::shared_ptr<fcitx::AsrSession>& session,
                    const std::shared_ptr<Outcome>& outcome) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (outcome->partials.load() == 0 &&
           !session->GetState()->workerDone.load() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return outcome->partials.load() > 0;
}

bool Run(const std::string& backend, const std::string& mode) {
    size_t writeSize = 4096;
    auto response = Response(backend, mode, writeSize);
    fcitxws::FakeWsServer server(std::move(response), writeSize,
                               mode == "final" ? 200ms : 0ms);
    if (!server.Start()) return false;
    auto outcome = std::make_shared<Outcome>();
    fcitx::AsrEngine::Config config;
    config.apiEndpoint = server.Url();
    config.apiKey = "fixture-key";
    config.modelName = backend == "realtime" ? "gpt-live-transcribe" : "bigmodel";
    auto errorCallback = [outcome, mode](const std::string&) {
        ++outcome->errors;
        if (mode == "error") throw std::runtime_error("private callback details");
    };
    std::shared_ptr<fcitx::AsrSession> session;
    if (backend == "realtime") {
        session = std::make_shared<fcitx::RealtimeAsrSession>(config, errorCallback, 73);
    } else {
        session = std::make_shared<fcitx::VolcengineAsrSession>(config, errorCallback, 73);
    }
    session->SetResultCallback([outcome, mode](const std::string& text,
                                             bool final, uint64_t sessionId) {
        if (final) {
            outcome->finalText = text;
            outcome->finalSessionId = sessionId;
            ++outcome->finals;
            if (mode == "final") throw std::runtime_error("private callback details");
        } else {
            ++outcome->partials;
            if (mode == "partial") throw std::runtime_error("private callback details");
            if (mode == "unknown") throw 7;
        }
    });
    session->StartWorker();
    bool healthyAfterError = true;
    if (backend == "realtime" && (mode == "final" || mode == "error")) {
        healthyAfterError = WaitForPartial(session, outcome);
        if (mode == "final") {
            session->End();
        } else {
            healthyAfterError = healthyAfterError && !session->GetState()->workerDone.load();
            session->Cancel(); // a recoverable API error must leave it running
        }
    }
    const bool completed = fcitx::WaitForWorkerCompletion(
        session->GetState()->workerDone, 2s);
    if (!completed) session->Cancel(); // cleanup must not mask failure to finish
    const auto joinStart = std::chrono::steady_clock::now();
    session->JoinWithTimeout(500ms);
    const auto joinTime = std::chrono::steady_clock::now() - joinStart;
    server.Stop();
    if (!completed || !healthyAfterError || joinTime >= 500ms ||
        !session->GetState()->finished.load()) return false;
    const bool cancelled = backend == "realtime" && mode == "error";
    if (outcome->finals.load() != (cancelled ? 0 : 1)) return false;
    if (!cancelled && outcome->finalSessionId.load() != 73) return false;
    if (mode == "empty-ack") {
        return outcome->errors.load() == 0 && outcome->partials.load() == 1 &&
               outcome->finalText == "fixture";
    }
    const std::string expectedFinal = mode == "final" ? "fixture" : "";
    return outcome->errors.load() == 1 && outcome->finalText == expectedFinal;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::string backend = argv[1];
    const std::string mode = argv[2];
    if ((backend != "realtime" && backend != "volcengine") ||
        (mode != "partial" && mode != "unknown" && mode != "error" &&
         mode != "final" && mode != "empty-ack") ||
        (backend == "realtime" && mode == "empty-ack")) return 2;
    if (!SupportsWebSocket()) return 77;
    const bool passed = Run(backend, mode);
    std::cerr << (passed ? "PASS: " : "FAIL: ") << backend << " " << mode << '\n';
    return passed ? 0 : 1;
}
