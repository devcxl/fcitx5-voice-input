#include "silero_vad.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include <fcitx-utils/log.h>

#ifdef ENABLE_SILERO_VAD
#include <onnxruntime_cxx_api.h>
#endif

namespace fcitx {

SileroVad::SileroVad(const std::string& modelPath) {
    Init(modelPath);
}

SileroVad::~SileroVad() {
#ifdef ENABLE_SILERO_VAD
    delete static_cast<Ort::MemoryInfo*>(memoryInfo_);
    delete static_cast<Ort::SessionOptions*>(options_);
    delete static_cast<Ort::Session*>(session_);
    delete static_cast<Ort::Env*>(env_);
#endif
}

bool SileroVad::Init(const std::string& modelPath) {
#ifdef ENABLE_SILERO_VAD
    if (!std::filesystem::exists(modelPath)) {
        FCITX_WARN() << "[voice-input:vad] Silero model not found: " << modelPath;
        return false;
    }

    if (ready_ && modelPath_ == modelPath) {
        Reset();
        return true;
    }

    try {
        std::unique_ptr<Ort::Env> env(
            new Ort::Env(OrtLoggingLevel::ORT_LOGGING_LEVEL_WARNING,
                         "fcitx5-voice-input-silero-vad"));
        std::unique_ptr<Ort::SessionOptions> options(
            new Ort::SessionOptions());
        options->SetIntraOpNumThreads(1);
        options->SetInterOpNumThreads(1);
        options->SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);

        std::unique_ptr<Ort::Session> session(
            new Ort::Session(*env, modelPath.c_str(), *options));

        std::unique_ptr<Ort::MemoryInfo> memoryInfo(
            new Ort::MemoryInfo(
                Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeCPU)));

        // 全部成功后才清理旧对象并转移所有权
        delete static_cast<Ort::Session*>(session_);
        session_ = nullptr;
        delete static_cast<Ort::SessionOptions*>(options_);
        options_ = nullptr;
        delete static_cast<Ort::Env*>(env_);
        env_ = nullptr;
        delete static_cast<Ort::MemoryInfo*>(memoryInfo_);
        memoryInfo_ = nullptr;

        env_ = env.release();
        options_ = options.release();
        session_ = session.release();
        memoryInfo_ = memoryInfo.release();

        modelPath_ = modelPath;
        Reset();
        ready_ = true;
        FCITX_INFO() << "[voice-input:vad] Silero VAD ready: " << modelPath;
        return true;
    } catch (const Ort::Exception& e) {
        FCITX_WARN() << "[voice-input:vad] Failed to load Silero model: "
                     << e.what();
        return false;
    }
#else
    FCITX_WARN() << "[voice-input:vad] ENABLE_SILERO_VAD not defined";
    return false;
#endif
}

void SileroVad::Reset() {
#ifdef ENABLE_SILERO_VAD
    std::fill(context_.begin(), context_.end(), 0.0f);
    std::fill(state_.begin(), state_.end(), 0.0f);
#endif
}

float SileroVad::Predict(const int16_t* pcm, size_t samples) {
#ifdef ENABLE_SILERO_VAD
    if (!ready_ || samples != kWindowSamples) {
        return -1.0f;
    }

    auto* session = static_cast<Ort::Session*>(session_);
    auto* memoryInfo = static_cast<Ort::MemoryInfo*>(memoryInfo_);

    // Convert int16 → float and prepend context
    static constexpr float kInt16ToFloat = 1.0f / 32768.0f;

    std::copy(context_.begin(), context_.end(), input_.begin());
    for (size_t i = 0; i < samples; ++i) {
        input_[kContextSamples + i] =
            static_cast<float>(pcm[i]) * kInt16ToFloat;
    }

    std::array<int64_t, 2> inputShape{1, kEffectiveSamples};
    std::array<int64_t, 3> stateShape{2, 1, 128};
    std::array<int64_t, 1> srShape{1};

    const char* inputNames[] = {"input", "state", "sr"};
    const char* outputNames[] = {"output", "stateN"};

    std::array<Ort::Value, 3> inputs{
        Ort::Value::CreateTensor<float>(
            *memoryInfo, input_.data(), input_.size(),
            inputShape.data(), inputShape.size()),
        Ort::Value::CreateTensor<float>(
            *memoryInfo, state_.data(), state_.size(),
            stateShape.data(), stateShape.size()),
        Ort::Value::CreateTensor<int64_t>(
            *memoryInfo, sr_.data(), sr_.size(),
            srShape.data(), srShape.size()),
    };

    try {
        auto outputs = session->Run(
            Ort::RunOptions{nullptr}, inputNames, inputs.data(), inputs.size(),
            outputNames, 2);

        float speechProbability =
            outputs[0].GetTensorMutableData<float>()[0];
        float* nextState = outputs[1].GetTensorMutableData<float>();
        std::memcpy(state_.data(), nextState,
                    state_.size() * sizeof(float));
        std::copy(input_.end() - kContextSamples, input_.end(),
                  context_.begin());

        return std::clamp(speechProbability, 0.0f, 1.0f);
    } catch (const Ort::Exception& e) {
        FCITX_WARN() << "[voice-input:vad] Silero inference failed: "
                     << e.what();
        ready_ = false;
        return -1.0f;
    }
#else
    return -1.0f;
#endif
}

} // namespace fcitx
