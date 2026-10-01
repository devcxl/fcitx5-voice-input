#include "chat_asr_request.h"

namespace fcitx {

Json::Value BuildChatAsrRequestBody(const std::string& model,
                                    const std::string& language,
                                    bool enableItn,
                                    const std::string& audioDataUrl) {
    Json::Value audioPart;
    audioPart["type"] = "input_audio";
    audioPart["input_audio"]["data"] = audioDataUrl;

    Json::Value userMessage;
    userMessage["role"] = "user";
    userMessage["content"].append(audioPart);

    Json::Value body;
    body["model"] = model;
    body["messages"].append(userMessage);
    body["stream"] = false;

    Json::Value asrOptions;
    if (!language.empty() && language != "auto") {
        asrOptions["language"] = language;
    }
    if (enableItn) {
        asrOptions["enable_itn"] = true;
    }
    if (!asrOptions.empty()) {
        body["asr_options"] = asrOptions;
    }

    return body;
}

} // namespace fcitx
