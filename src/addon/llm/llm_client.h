#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "llm/llm_request_cancellation.h"

namespace fcitx {

class LLMClient {
public:
    struct Config {
        std::string endpoint;
        std::string apiKey;
        std::string model;
        std::string systemPrompt;
    };

    LLMClient(Config config);
    ~LLMClient();

    LLMClient(const LLMClient&) = delete;
    LLMClient& operator=(const LLMClient&) = delete;

    // 非流式处理：仅在请求仍属于活动 generation 时发布成功结果。
    void Process(const std::string& text, uint64_t generation,
                 std::function<void(const std::string&)> onComplete);

    // Streaming: calls onToken with each incremental chunk,
    // onComplete with the full accumulated text.
    // Both callbacks run on the calling thread.
    void ProcessStream(const std::string& text, uint64_t generation,
                       std::function<void(const std::string&)> onToken,
                       std::function<void(const std::string&)> onComplete);

    /// 激活新会话；只有 generation 匹配的请求可执行和发布。
    void Activate(uint64_t generation) { cancellation_.Activate(generation); }

    /// 取消活动会话，并等待已进入发布门的短回调完成。
    void Cancel() { cancellation_.Cancel(); }

private:
    Config config_;
    LLMRequestCancellation cancellation_;
};

} // namespace fcitx
