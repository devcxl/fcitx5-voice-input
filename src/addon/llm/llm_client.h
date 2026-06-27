#pragma once

#include <string>
#include <functional>
#include <vector>

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

    // Non-streaming: returns processed text on success, empty on failure.
    std::string Process(const std::string& text);

    // Streaming: calls onToken with each incremental chunk,
    // onComplete with the full accumulated text.
    // Both callbacks run on the calling thread.
    void ProcessStream(const std::string& text,
                       std::function<void(const std::string&)> onToken,
                       std::function<void(const std::string&)> onComplete);

private:
    Config config_;
};

} // namespace fcitx
