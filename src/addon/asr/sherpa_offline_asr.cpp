#include "sherpa_offline_asr.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <fcitx-utils/log.h>

#include "utils/sherpa_paths.h"

namespace fcitx {

#ifdef HAVE_SHERPA_ONNX

// -------------------------------------------------------------
// SherpaOfflineRecognizerHolder 实现
// -------------------------------------------------------------
SherpaOfflineRecognizerHolder::SherpaOfflineRecognizerHolder(
    const SherpaOnnxOfflineRecognizer* recognizer)
    : recognizer_(recognizer) {}

SherpaOfflineRecognizerHolder::~SherpaOfflineRecognizerHolder() {
    if (recognizer_) {
        SherpaOnnxDestroyOfflineRecognizer(recognizer_);
        recognizer_ = nullptr;
    }
}

std::string SherpaOfflineRecognizerHolder::Transcribe(
    const float* pcm, size_t frames, const std::atomic<bool>& cancelled) {
    if (!recognizer_ || !pcm || frames == 0) return "";

    const SherpaOnnxOfflineStream* stream = nullptr;
    const SherpaOnnxOfflineRecognizerResult* result = nullptr;
    std::string text;

    try {
        stream = SherpaOnnxCreateOfflineStream(recognizer_);
        if (!stream) return "";

        SherpaOnnxAcceptWaveformOffline(stream, 16000, pcm,
                                        static_cast<int32_t>(frames));

        // 取消检查：解码前放弃，避免长时间阻塞 worker 线程
        if (cancelled.load(std::memory_order_acquire)) {
            SherpaOnnxDestroyOfflineStream(stream);
            return "";
        }

        SherpaOnnxDecodeOfflineStream(recognizer_, stream);
        result = SherpaOnnxGetOfflineStreamResult(stream);
        if (result && result->text) {
            text = result->text;
        }
    } catch (const std::exception& e) {
        FCITX_ERROR() << "[voice-input:sherpa_offline] Transcribe exception: "
                      << e.what();
    } catch (...) {
        FCITX_ERROR()
            << "[voice-input:sherpa_offline] Transcribe unknown exception";
    }

    if (result) {
        try {
            SherpaOnnxDestroyOfflineRecognizerResult(result);
        } catch (...) {}
    }
    if (stream) {
        try {
            SherpaOnnxDestroyOfflineStream(stream);
        } catch (...) {}
    }
    return text;
}

// -------------------------------------------------------------
// SherpaOfflineAsrSession 实现
// -------------------------------------------------------------
SherpaOfflineAsrSession::SherpaOfflineAsrSession(
    std::shared_ptr<SherpaOfflineRecognizerHolder> holder,
    AsrSession::ErrorCallback errorCb,
    uint64_t sessionId)
    : holder_(std::move(holder)) {
    errorCb_ = std::move(errorCb);
    state_->sessionId = sessionId;
    state_->workerDone.store(true, std::memory_order_release);
}

SherpaOfflineAsrSession::~SherpaOfflineAsrSession() {
    Cancel();
    JoinWithTimeout(std::chrono::milliseconds(2000));
}

void SherpaOfflineAsrSession::FeedAudio(const float* pcm, size_t frames) {
    if (!pcm || frames == 0 ||
        state_->cancelled.load(std::memory_order_relaxed) ||
        state_->finished.load(std::memory_order_relaxed)) {
        return;
    }

    std::lock_guard<std::mutex> lock(bufferMutex_);
    if (endRequested_) return;
    pcmBuffer_.insert(pcmBuffer_.end(), pcm, pcm + frames);
}

void SherpaOfflineAsrSession::End() {
    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        if (endRequested_) return;
        endRequested_ = true;
    }
    state_->finished.store(true, std::memory_order_release);

    // 契约要求调用方先 StartWorker()；此处兜底，避免 End 后无人回收缓冲。
    if (!workerThread_) StartWorker();
    bufferCv_.notify_all();
}

void SherpaOfflineAsrSession::Cancel() {
    state_->cancelled.store(true, std::memory_order_release);
    bufferCv_.notify_all();
}

void SherpaOfflineAsrSession::JoinWithTimeout(
    std::chrono::milliseconds timeout) {
    if (workerThread_ && workerThread_->joinable()) {
        if (workerThread_->get_id() == std::this_thread::get_id()) {
            workerThread_->detach();
        } else if (WaitForWorkerCompletion(state_->workerDone, timeout)) {
            workerThread_->join();
        } else {
            FCITX_WARN() << "[voice-input:sherpa_offline] Join timeout session="
                         << state_->sessionId;
            Cancel();
            workerThread_->detach();
        }
    }
    workerThread_.reset();
}

void SherpaOfflineAsrSession::StartWorker() {
    if (workerThread_) return;

    // 用 shared_from_this 保活会话：worker 可能因解码较慢在 JoinWithTimeout
    // 超时后被 detach，此时会话对象仍必须存活。
    auto self = std::static_pointer_cast<SherpaOfflineAsrSession>(
        shared_from_this());
    state_->workerDone.store(false, std::memory_order_release);
    workerThread_ = std::make_unique<std::thread>([self]() {
        self->WorkerLoop();
        self->state_->workerDone.store(true, std::memory_order_release);
    });
}

void SherpaOfflineAsrSession::WorkerLoop() {
    std::vector<float> pcm;
    {
        std::unique_lock<std::mutex> lock(bufferMutex_);
        bufferCv_.wait(lock, [this] {
            return endRequested_ ||
                   state_->cancelled.load(std::memory_order_acquire);
        });
        if (state_->cancelled.load(std::memory_order_acquire)) return;
        pcm = std::move(pcmBuffer_);
    }

    TranscribeUtterance(std::move(pcm));
}

void SherpaOfflineAsrSession::TranscribeUtterance(std::vector<float> pcm) {
    auto state = state_;
    auto cb = resultCb_;
    auto ecb = errorCb_;

    if (!holder_) {
        if (ecb) ecb("SherpaOffline recognizer is null");
        if (cb) cb("", true, state->sessionId);
        return;
    }

    if (pcm.empty()) {
        // 空音频也要交出终态：否则该语音段只能等保序闸门停滞放行，
        // 后续结果会被无谓地扣住。
        FCITX_WARN() << "[voice-input:sherpa_offline] Empty audio session="
                     << state->sessionId;
        if (cb) cb("", true, state->sessionId);
        return;
    }

    std::string text =
        holder_->Transcribe(pcm.data(), pcm.size(), state->cancelled);

    if (state->cancelled.load(std::memory_order_acquire)) {
        FCITX_DEBUG() << "[voice-input:sherpa_offline] Cancelled session="
                      << state->sessionId;
        return;
    }

    text.erase(0, text.find_first_not_of(" \t\n\r"));
    if (!text.empty()) {
        text.erase(text.find_last_not_of(" \t\n\r") + 1);
    }

    if (text.empty()) {
        FCITX_WARN() << "[voice-input:sherpa_offline] Empty result session="
                     << state->sessionId;
        if (cb) cb("", true, state->sessionId);
        return;
    }

    FCITX_DEBUG() << "[voice-input:sherpa_offline] final session="
                  << state->sessionId << " len=" << text.size();
    if (cb) cb(text, true, state->sessionId);
}

// -------------------------------------------------------------
// SherpaOfflineAsrEngine 实现
// -------------------------------------------------------------
SherpaOfflineAsrEngine::SherpaOfflineAsrEngine() = default;

SherpaOfflineAsrEngine::~SherpaOfflineAsrEngine() {
    CancelAllSessions();
}

bool SherpaOfflineAsrEngine::Init(const Config& config) {
    config_ = config;
    SherpaModelPaths paths = ResolveSherpaModelPaths(
        config.modelPath,
        {config.encoderPath, config.decoderPath, config.joinerPath,
         config.tokensPath},
        "voice-input:sherpa_offline");
    if (!paths.valid) {
        FCITX_ERROR() << "[voice-input:sherpa_offline] Failed to find valid model files. "
                      << "Please check encoder, decoder, joiner, tokens.txt.";
        return false;
    }

    SherpaOnnxOfflineRecognizerConfig c_cfg;
    std::memset(&c_cfg, 0, sizeof(c_cfg));

    c_cfg.feat_config.sample_rate = 16000;
    c_cfg.feat_config.feature_dim = 80;

    c_cfg.model_config.transducer.encoder = paths.encoder.c_str();
    c_cfg.model_config.transducer.decoder = paths.decoder.c_str();
    c_cfg.model_config.transducer.joiner = paths.joiner.c_str();
    c_cfg.model_config.tokens = paths.tokens.c_str();
    c_cfg.model_config.num_threads = std::max(1, config.numThreads);
    c_cfg.model_config.provider = "cpu";

    // X-ASR 词表里汉字只有 "▁汉字" 形态（每个汉字单独成词），热词需要逐字拆分
    // 并用 bpe 编码；常规中文模型仍是 cjkchar。两种情况下都不配置 bpe.model：
    // Ssentencepiece 期望 "token score" 文本词表，传二进制 proto 会 exit(-1)。
    constexpr const char* kTag = "voice-input:sherpa_offline";
    c_cfg.model_config.modeling_unit = "cjkchar";
    c_cfg.max_active_paths = 8;
    c_cfg.decoding_method = "greedy_search";

    std::string hotwordsFile;
    std::string bpeVocab;
    const std::string hotwordsPath =
        ResolveSherpaHotwordsFile(config.hotwordsFile, kTag);
    if (!hotwordsPath.empty()) {
        const HotwordEncoding encoding = DetectHotwordEncoding(paths.tokens);
        hotwordsFile = PrepareSherpaHotwordsFile(hotwordsPath, encoding, kTag);
        if (!hotwordsFile.empty()) {
            if (encoding == HotwordEncoding::CjkChar) {
                c_cfg.model_config.modeling_unit = "cjkchar";
            } else {
                c_cfg.model_config.modeling_unit = "bpe";
                bpeVocab = ResolveSherpaBpeVocab(paths.tokens);
                c_cfg.model_config.bpe_vocab = bpeVocab.c_str();
            }
            c_cfg.hotwords_file = hotwordsFile.c_str();
            c_cfg.hotwords_score =
                config.hotwordsScore > 0 ? config.hotwordsScore : 2.0f;
            c_cfg.decoding_method = "modified_beam_search";
            FCITX_INFO() << "[" << kTag << "] Hotwords enabled: " << hotwordsFile
                         << " encoding="
                         << (encoding == HotwordEncoding::CjkChar ? "cjkchar"
                             : encoding == HotwordEncoding::BpePrefixed
                                 ? "bpe(per-char)"
                                 : "bpe")
                         << " score=" << c_cfg.hotwords_score;
        }
    }

    const SherpaOnnxOfflineRecognizer* rawRecognizer = nullptr;
    try {
        rawRecognizer = SherpaOnnxCreateOfflineRecognizer(&c_cfg);
    } catch (const std::exception& e) {
        FCITX_ERROR() << "[voice-input:sherpa_offline] Exception during SherpaOnnxCreateOfflineRecognizer: "
                      << e.what();
        rawRecognizer = nullptr;
    } catch (...) {
        FCITX_ERROR() << "[voice-input:sherpa_offline] Unknown exception during SherpaOnnxCreateOfflineRecognizer";
        rawRecognizer = nullptr;
    }

    if (!rawRecognizer) {
        FCITX_ERROR() << "[voice-input:sherpa_offline] SherpaOnnxCreateOfflineRecognizer failed! Check model paths.";
        return false;
    }

    holder_ = std::make_shared<SherpaOfflineRecognizerHolder>(rawRecognizer);
    FCITX_INFO() << "[voice-input:sherpa_offline] Initialized successfully. encoder="
                 << paths.encoder << " decoder=" << paths.decoder
                 << " joiner=" << paths.joiner
                 << " threads=" << c_cfg.model_config.num_threads;
    return true;
}

AsrSessionStart SherpaOfflineAsrEngine::StartSession() {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto cancelledId = CancelOldestSessionIfLimitReachedLocked();

    if (!holder_) {
        FCITX_ERROR() << "[voice-input:sherpa_offline] Cannot start session: holder is null";
        return {nullptr, cancelledId};
    }

    const uint64_t sid = NextSessionId();
    auto session =
        std::make_shared<SherpaOfflineAsrSession>(holder_, errorCb_, sid);
    session->SetResultCallback(resultCb_);
    sessions_[sid] = session;

    return {session, cancelledId};
}

#else // !HAVE_SHERPA_ONNX

SherpaOfflineAsrSession::SherpaOfflineAsrSession(AsrSession::ErrorCallback errorCb,
                                                 uint64_t sessionId) {
    errorCb_ = std::move(errorCb);
    state_->sessionId = sessionId;
    state_->workerDone.store(true, std::memory_order_release);
}

SherpaOfflineAsrSession::~SherpaOfflineAsrSession() = default;
void SherpaOfflineAsrSession::FeedAudio(const float*, size_t) {}
void SherpaOfflineAsrSession::End() {}
void SherpaOfflineAsrSession::Cancel() {}
void SherpaOfflineAsrSession::JoinWithTimeout(std::chrono::milliseconds) {}
void SherpaOfflineAsrSession::StartWorker() {
    if (errorCb_) errorCb_("Built without sherpa-onnx support");
}

SherpaOfflineAsrEngine::SherpaOfflineAsrEngine() = default;
SherpaOfflineAsrEngine::~SherpaOfflineAsrEngine() = default;

bool SherpaOfflineAsrEngine::Init(const Config&) {
    FCITX_WARN() << "[voice-input:sherpa_offline] Built without sherpa-onnx support.";
    return false;
}

AsrSessionStart SherpaOfflineAsrEngine::StartSession() {
    return {nullptr, std::nullopt};
}

#endif // HAVE_SHERPA_ONNX

} // namespace fcitx
