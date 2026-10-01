#include <iostream>

#include <json/json.h>

#include "asr/utils/chat_asr_request.h"

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

const char* const kDataUrl = "data:audio/wav;base64,QUJD";

bool TestFullRequest() {
    const Json::Value body =
        fcitx::BuildChatAsrRequestBody("mimo-v2.5-asr", "zh", true, kDataUrl);
    return Check(body["model"].asString() == "mimo-v2.5-asr",
                 "model must be preserved") &&
           Check(!body["stream"].asBool(), "stream must be false") &&
           Check(body["messages"][0]["role"].asString() == "user",
                 "user message required") &&
           Check(body["messages"][0]["content"][0]["type"].asString() ==
                     "input_audio",
                 "audio content part required") &&
           Check(body["messages"][0]["content"][0]["input_audio"]["data"]
                     .asString() == kDataUrl,
                 "audio data URL must be preserved") &&
           Check(body["asr_options"]["language"].asString() == "zh",
                 "explicit language must be sent") &&
           Check(body["asr_options"]["enable_itn"].asBool(),
                 "enable_itn must be sent when requested");
}

bool TestAutoLanguageOmitted() {
    const Json::Value body =
        fcitx::BuildChatAsrRequestBody("mimo-v2.5-asr", "auto", true, kDataUrl);
    return Check(!body["asr_options"].isMember("language"),
                 "auto language must be omitted") &&
           Check(body["asr_options"]["enable_itn"].asBool(),
                 "enable_itn must survive auto language");
}

bool TestItnDisabled() {
    const Json::Value body =
        fcitx::BuildChatAsrRequestBody("mimo-v2.5-asr", "zh", false, kDataUrl);
    return Check(body["asr_options"]["language"].asString() == "zh",
                 "language must be sent without ITN") &&
           Check(!body["asr_options"].isMember("enable_itn"),
                 "enable_itn must be omitted when disabled");
}

bool TestEmptyAsrOptionsOmitted() {
    const Json::Value body =
        fcitx::BuildChatAsrRequestBody("mimo-v2.5-asr", "auto", false, kDataUrl);
    return Check(!body.isMember("asr_options"),
                 "empty asr_options must be omitted entirely");
}

} // namespace

int main() {
    const bool ok = TestFullRequest() && TestAutoLanguageOmitted() &&
                    TestItnDisabled() && TestEmptyAsrOptionsOmitted();
    if (!ok) return 1;
    std::cout << "chat_asr_request_test passed\n";
    return 0;
}
