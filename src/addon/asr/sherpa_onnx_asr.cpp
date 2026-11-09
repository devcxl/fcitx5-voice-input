#include "sherpa_onnx_asr.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <fcitx-utils/log.h>

#include "utils/sherpa_paths.h"

namespace fcitx {

#ifdef HAVE_SHERPA_ONNX

// -------------------------------------------------------------
// SherpaRecognizerHolder 实现
// -------------------------------------------------------------
SherpaRecognizerHolder::SherpaRecognizerHolder(const SherpaOnnxOnlineRecognizer* recognizer)
    : recognizer_(recognizer) {}

SherpaRecognizerHolder::~SherpaRecognizerHolder() {
    if (recognizer_) {
        SherpaOnnxDestroyOnlineRecognizer(recognizer_);
        recognizer_ = nullptr;
    }
}

const SherpaOnnxOnlineStream* SherpaRecognizerHolder::CreateStream() const {
    if (!recognizer_) return nullptr;
    try {
        return SherpaOnnxCreateOnlineStream(recognizer_);
    } catch (const std::exception& e) {
        FCITX_ERROR() << "[voice-input:sherpa_onnx] CreateStream exception: " << e.what();
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

void SherpaRecognizerHolder::DestroyStream(const SherpaOnnxOnlineStream* stream) const {
    if (stream) {
        try {
            SherpaOnnxDestroyOnlineStream(stream);
        } catch (...) {}
    }
}

bool SherpaRecognizerHolder::IsReady(const SherpaOnnxOnlineStream* stream) {
    if (!recognizer_ || !stream) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        return SherpaOnnxIsOnlineStreamReady(recognizer_, stream) != 0;
    } catch (const std::exception& e) {
        FCITX_ERROR() << "[voice-input:sherpa_onnx] IsReady exception: " << e.what();
        return false;
    } catch (...) {
        return false;
    }
}

void SherpaRecognizerHolder::Decode(const SherpaOnnxOnlineStream* stream) {
    if (!recognizer_ || !stream) return;
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        SherpaOnnxDecodeOnlineStream(recognizer_, stream);
    } catch (const std::exception& e) {
        FCITX_ERROR() << "[voice-input:sherpa_onnx] Decode exception: " << e.what();
    } catch (...) {}
}

std::string SherpaRecognizerHolder::GetResult(const SherpaOnnxOnlineStream* stream) {
    if (!recognizer_ || !stream) return "";
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        const auto* res = SherpaOnnxGetOnlineStreamResult(recognizer_, stream);
        if (!res) return "";
        std::string text = (res->text != nullptr) ? res->text : "";
        SherpaOnnxDestroyOnlineRecognizerResult(res);
        return text;
    } catch (const std::exception& e) {
        FCITX_ERROR() << "[voice-input:sherpa_onnx] GetResult exception: " << e.what();
        return "";
    } catch (...) {
        return "";
    }
}

void SherpaRecognizerHolder::ResetStream(const SherpaOnnxOnlineStream* stream) {
    if (!recognizer_ || !stream) return;
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        SherpaOnnxOnlineStreamReset(recognizer_, stream);
    } catch (...) {}
}

// -------------------------------------------------------------
// SherpaOnnxAsrSession 实现
// -------------------------------------------------------------
SherpaOnnxAsrSession::SherpaOnnxAsrSession(
    std::shared_ptr<SherpaRecognizerHolder> holder,
    AsrSession::ErrorCallback errorCb,
    uint64_t sessionId)
    : holder_(std::move(holder))
    , audioChunks_(std::make_shared<ThreadSafeQueue<std::vector<float>>>(128)) {
    errorCb_ = std::move(errorCb);
    state_->sessionId = sessionId;
    state_->workerDone.store(true, std::memory_order_release);

    if (holder_) {
        stream_ = holder_->CreateStream();
    }
}

SherpaOnnxAsrSession::~SherpaOnnxAsrSession() {
    Cancel();
    JoinWithTimeout(std::chrono::milliseconds(2000));
    if (holder_ && stream_) {
        holder_->DestroyStream(stream_);
        stream_ = nullptr;
    }
}

void SherpaOnnxAsrSession::FeedAudio(const float* pcm, size_t frames) {
    if (!pcm || frames == 0 || state_->cancelled.load(std::memory_order_relaxed) ||
        state_->finished.load(std::memory_order_relaxed)) {
        return;
    }
    if (audioChunks_) {
        audioChunks_->Push(std::vector<float>(pcm, pcm + frames));
    }
}

void SherpaOnnxAsrSession::End() {
    state_->finished.store(true, std::memory_order_release);
    if (audioChunks_) {
        audioChunks_->Push(std::vector<float>());
    }
}

void SherpaOnnxAsrSession::Cancel() {
    state_->cancelled.store(true, std::memory_order_release);
    if (audioChunks_) {
        audioChunks_->Stop();
    }
}

void SherpaOnnxAsrSession::JoinWithTimeout(std::chrono::milliseconds timeout) {
    if (workerThread_ && workerThread_->joinable()) {
        if (workerThread_->get_id() == std::this_thread::get_id()) {
            workerThread_->detach();
        } else if (WaitForWorkerCompletion(state_->workerDone, timeout)) {
            workerThread_->join();
        } else {
            FCITX_WARN() << "[voice-input:sherpa_onnx] Join timeout session="
                         << state_->sessionId;
            Cancel();
            workerThread_->detach();
        }
    }
    workerThread_.reset();
}

void SherpaOnnxAsrSession::StartWorker() {
    if (workerThread_) return;
    auto self = std::static_pointer_cast<SherpaOnnxAsrSession>(shared_from_this());
    state_->workerDone.store(false, std::memory_order_release);
    workerThread_ = std::make_unique<std::thread>([self]() {
        self->WorkerLoop();
        self->state_->workerDone.store(true, std::memory_order_release);
    });
}

void SherpaOnnxAsrSession::WorkerLoop() {
    uint64_t sessionId = state_->sessionId;
    auto cb = resultCb_;
    auto ecb = errorCb_;
    auto audioChunks = audioChunks_;

    if (!holder_ || !stream_) {
        if (ecb) ecb("SherpaOnnx stream is null");
        if (cb) cb("", true, sessionId);
        return;
    }

    std::string lastText;

    while (!state_->cancelled.load(std::memory_order_acquire)) {
        std::vector<float> chunk = audioChunks->Pop();
        if (state_->cancelled.load(std::memory_order_acquire)) {
            break;
        }

        if (chunk.empty()) {
            if (state_->finished.load(std::memory_order_acquire)) {
                break;
            }
            continue;
        }

        SherpaOnnxOnlineStreamAcceptWaveform(
            stream_, 16000, chunk.data(), static_cast<int32_t>(chunk.size()));

        while (!state_->cancelled.load(std::memory_order_acquire) &&
               holder_->IsReady(stream_)) {
            holder_->Decode(stream_);
        }

        if (state_->cancelled.load(std::memory_order_acquire)) break;

        std::string currentText = holder_->GetResult(stream_);
        if (!currentText.empty() && currentText != lastText) {
            lastText = currentText;
            if (cb) {
                cb(currentText, /*isFinal=*/false, sessionId);
            }
        }
    }

    // 录音正常结束（End），刷新尾部音频并触发 final
    if (!state_->cancelled.load(std::memory_order_acquire)) {
        SherpaOnnxOnlineStreamInputFinished(stream_);
        while (!state_->cancelled.load(std::memory_order_acquire) &&
               holder_->IsReady(stream_)) {
            holder_->Decode(stream_);
        }

        std::string finalText = holder_->GetResult(stream_);
        if (finalText.empty()) {
            finalText = lastText;
        }
        if (cb) {
            cb(finalText, /*isFinal=*/true, sessionId);
        }
    }
}

// -------------------------------------------------------------
// SherpaOnnxAsrEngine 实现
// -------------------------------------------------------------
SherpaOnnxAsrEngine::SherpaOnnxAsrEngine() = default;

SherpaOnnxAsrEngine::~SherpaOnnxAsrEngine() {
    CancelAllSessions();
}

bool SherpaOnnxAsrEngine::Init(const Config& config) {
    config_ = config;
    SherpaModelPaths paths = ResolveSherpaModelPaths(
        config.modelPath,
        {config.encoderPath, config.decoderPath, config.joinerPath,
         config.tokensPath},
        "voice-input:sherpa_onnx");
    if (!paths.valid) {
        FCITX_ERROR() << "[voice-input:sherpa_onnx] Failed to find valid model files. "
                      << "Please check encoder, decoder, joiner, tokens.txt.";
        return false;
    }

    SherpaOnnxOnlineRecognizerConfig c_cfg;
    std::memset(&c_cfg, 0, sizeof(c_cfg));

    c_cfg.model_config.transducer.encoder = paths.encoder.c_str();
    c_cfg.model_config.transducer.decoder = paths.decoder.c_str();
    c_cfg.model_config.transducer.joiner  = paths.joiner.c_str();
    c_cfg.model_config.tokens             = paths.tokens.c_str();
    c_cfg.model_config.modeling_unit      = "cjkchar";
    c_cfg.model_config.num_threads        = std::max(1, config.numThreads);
    c_cfg.model_config.provider           = "cpu";
    c_cfg.max_active_paths                = 8;

    c_cfg.feat_config.sample_rate         = 16000;
    c_cfg.feat_config.feature_dim         = 80;

    // 断句由 Pipeline 的 Silero VAD 统一控制，关闭内置断句避免双重切分冲突
    c_cfg.enable_endpoint                 = 0;

    // 如果未显式配置热词路径，自动探测默认位置
    const std::string hotwordsPath = ResolveSherpaHotwordsFile(
        config.hotwordsFile, "voice-input:sherpa_onnx");

    if (!hotwordsPath.empty()) {
        c_cfg.hotwords_file               = hotwordsPath.c_str();
        c_cfg.hotwords_score              = config.hotwordsScore > 0 ? config.hotwordsScore : 2.0f;
        c_cfg.decoding_method             = "modified_beam_search";
        FCITX_INFO() << "[voice-input:sherpa_onnx] Hotwords enabled: " << hotwordsPath
                     << " score=" << c_cfg.hotwords_score;
    } else {
        c_cfg.decoding_method             = "greedy_search";
    }

    const SherpaOnnxOnlineRecognizer* rawRecognizer = nullptr;
    try {
        rawRecognizer = SherpaOnnxCreateOnlineRecognizer(&c_cfg);
    } catch (const std::exception& e) {
        FCITX_ERROR() << "[voice-input:sherpa_onnx] Exception during SherpaOnnxCreateOnlineRecognizer: "
                      << e.what();
        rawRecognizer = nullptr;
    } catch (...) {
        FCITX_ERROR() << "[voice-input:sherpa_onnx] Unknown exception during SherpaOnnxCreateOnlineRecognizer";
        rawRecognizer = nullptr;
    }

    if (!rawRecognizer) {
        FCITX_ERROR() << "[voice-input:sherpa_onnx] SherpaOnnxCreateOnlineRecognizer failed! Check model paths.";
        return false;
    }

    holder_ = std::make_shared<SherpaRecognizerHolder>(rawRecognizer);
    FCITX_INFO() << "[voice-input:sherpa_onnx] Initialized successfully. encoder=" << paths.encoder
                 << " decoder=" << paths.decoder << " joiner=" << paths.joiner
                 << " threads=" << c_cfg.model_config.num_threads;
    return true;
}

AsrSessionStart SherpaOnnxAsrEngine::StartSession() {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto cancelledId = CancelOldestSessionIfLimitReachedLocked();

    if (!holder_) {
        FCITX_ERROR() << "[voice-input:sherpa_onnx] Cannot start session: holder is null";
        return {nullptr, cancelledId};
    }

    const uint64_t sid = NextSessionId();
    auto session = std::make_shared<SherpaOnnxAsrSession>(holder_, errorCb_, sid);
    session->SetResultCallback(resultCb_);
    sessions_[sid] = session;

    return {session, cancelledId};
}

#else // !HAVE_SHERPA_ONNX

SherpaOnnxAsrSession::SherpaOnnxAsrSession(AsrSession::ErrorCallback errorCb, uint64_t sessionId)
    : audioChunks_(std::make_shared<ThreadSafeQueue<std::vector<float>>>(128)) {
    errorCb_ = std::move(errorCb);
    state_->sessionId = sessionId;
    state_->workerDone.store(true, std::memory_order_release);
}

SherpaOnnxAsrSession::~SherpaOnnxAsrSession() = default;
void SherpaOnnxAsrSession::FeedAudio(const float*, size_t) {}
void SherpaOnnxAsrSession::End() {}
void SherpaOnnxAsrSession::Cancel() {}
void SherpaOnnxAsrSession::JoinWithTimeout(std::chrono::milliseconds) {}
void SherpaOnnxAsrSession::StartWorker() {
    if (errorCb_) errorCb_("Built without sherpa-onnx support");
}

SherpaOnnxAsrEngine::SherpaOnnxAsrEngine() = default;
SherpaOnnxAsrEngine::~SherpaOnnxAsrEngine() = default;

bool SherpaOnnxAsrEngine::Init(const Config&) {
    FCITX_WARN() << "[voice-input:sherpa_onnx] Built without sherpa-onnx support.";
    return false;
}

AsrSessionStart SherpaOnnxAsrEngine::StartSession() {
    return {nullptr, std::nullopt};
}

#endif // HAVE_SHERPA_ONNX

} // namespace fcitx
