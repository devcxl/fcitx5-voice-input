#pragma once

#include <string>
#include <fcitx-utils/key.h>
#include <nlohmann/json.hpp>

namespace fcitx {

/**
 * Runtime configuration for the voice input addon.
 *
 * Loaded from JSON config (stored alongside the addon).
 */
struct VoiceInputConfig {
    // ── Key binding ───────────────────────────────────────────────
    Key triggerKey = Key("Alt_R");

    // ── ASR backend ─────────────────────────────────────────────────
    /// Which ASR engine: "openai" (default) or "sherpa-onnx"
    std::string asrBackend = "openai";

    // ── OpenAI-compatible ASR ──────────────────────────────────────
    /// API endpoint base URL (e.g. "https://api.openai.com/v1")
    std::string openaiEndpoint = "https://api.openai.com/v1";
    /// API key for authentication
    std::string openaiApiKey;
    /// Model name (e.g. "whisper-1", "whisper-large-v3")
    std::string openaiModel = "whisper-1";
    /// Language hint (ISO 639-1, e.g. "zh" for Chinese)
    std::string openaiLanguage = "zh";

    // ── Sherpa-onnx (local ASR) ───────────────────────────────────
    /// Path to model directory
    std::string modelPath;
    /// Model name/preset
    std::string modelName;
    /// Number of inference threads
    int numThreads = 4;

    // ── Voice Activity Detection ──────────────────────────────────
    /// VAD sensitivity threshold (0.0–1.0, lower = more sensitive)
    float vadThreshold = 0.3f;
    /// Silence duration in ms before recording stops
    int silenceThresholdMs = 800;

    // ── Scene / LLM post-processing (reserved) ────────────────────
    std::string activeScene;
    std::string llmEndpoint;
    std::string llmApiKey;

    // ── Serialization ─────────────────────────────────────────────
    nlohmann::json ToJson() const;

    static VoiceInputConfig FromJson(const nlohmann::json& j);
    static VoiceInputConfig Defaults() { return VoiceInputConfig{}; }
};

} // namespace fcitx
