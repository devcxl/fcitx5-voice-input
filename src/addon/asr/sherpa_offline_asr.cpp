#include "sherpa_offline_asr.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include <fcitx-utils/log.h>

namespace fcitx {

#ifdef HAVE_SHERPA_ONNX

namespace {

struct ResolvedPaths {
    std::string encoder;
    std::string decoder;
    std::string joiner;
    std::string tokens;
    bool valid = false;
};

std::string FindMatchingModelFile(const std::string& dir, const std::string& prefix) {
    namespace fs = std::filesystem;
    std::string int8_candidate;
    std::string fp32_candidate;

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        std::string filename = entry.path().filename().string();
        if (filename.rfind(prefix, 0) == 0 && filename.rfind(".onnx") != std::string::npos) {
            if (filename.rfind(".int8.onnx") != std::string::npos) {
                int8_candidate = entry.path().string();
            } else {
                fp32_candidate = entry.path().string();
            }
        }
    }
    return !int8_candidate.empty() ? int8_candidate : fp32_candidate;
}

ResolvedPaths ResolveSherpaOfflineModelPaths(const AsrEngine::Config& config) {
    namespace fs = std::filesystem;
    ResolvedPaths res;

    // 1. 用户显式指定所有文件路径，直接校验
    if (!config.encoderPath.empty() && !config.decoderPath.empty() &&
        !config.joinerPath.empty() && !config.tokensPath.empty()) {
        std::error_code ec;
        if (fs::exists(config.encoderPath, ec) &&
            fs::exists(config.decoderPath, ec) &&
            fs::exists(config.joinerPath, ec) &&
            fs::exists(config.tokensPath, ec)) {
            res.encoder = config.encoderPath;
            res.decoder = config.decoderPath;
            res.joiner = config.joinerPath;
            res.tokens = config.tokensPath;
            res.valid = true;
            return res;
        }
    }

    // 2. 候选模型根目录列表（与在线 sherpa 后端一致）
    std::vector<std::string> baseDirs;
    if (!config.modelPath.empty()) {
        baseDirs.push_back(config.modelPath);
    }

    const char* home = std::getenv("HOME");
    const char* xdgData = std::getenv("XDG_DATA_HOME");
    std::string userShare = xdgData ? std::string(xdgData)
                                    : (home ? std::string(home) + "/.local/share" : "");
    if (!userShare.empty()) {
        baseDirs.push_back(userShare + "/fcitx5/voice-input/models");
    }
    baseDirs.push_back("/usr/local/share/fcitx5/voice-input/models");

    auto checkDir = [&](const std::string& dir) -> bool {
        std::error_code ec;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
            return false;
        }

        std::string enc = config.encoderPath.empty() ? FindMatchingModelFile(dir, "encoder") : config.encoderPath;
        std::string dec = config.decoderPath.empty() ? FindMatchingModelFile(dir, "decoder") : config.decoderPath;
        std::string joi = config.joinerPath.empty() ? FindMatchingModelFile(dir, "joiner") : config.joinerPath;
        std::string tok = config.tokensPath.empty() ? (fs::exists(fs::path(dir) / "tokens.txt", ec) ? (fs::path(dir) / "tokens.txt").string() : "") : config.tokensPath;

        if (!enc.empty() && !dec.empty() && !joi.empty() && !tok.empty()) {
            res.encoder = enc;
            res.decoder = dec;
            res.joiner = joi;
            res.tokens = tok;
            res.valid = true;
            FCITX_INFO() << "[voice-input:sherpa_offline] Found valid model directory: " << dir;
            return true;
        }
        return false;
    };

    for (const auto& base : baseDirs) {
        std::error_code ec;
        if (!fs::exists(base, ec) || !fs::is_directory(base, ec)) {
            continue;
        }

        // 优先检查根目录自身
        if (checkDir(base)) {
            return res;
        }

        // 遍历根目录下的各子文件夹进行智能探测
        for (const auto& entry : fs::directory_iterator(base, ec)) {
            if (ec) break;
            if (entry.is_directory(ec)) {
                if (checkDir(entry.path().string())) {
                    return res;
                }
            }
        }
    }

    return res;
}

} // namespace

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

const SherpaOnnxOfflineStream* SherpaOfflineRecognizerHolder::CreateStream() const {
    if (!recognizer_) return nullptr;
    try {
        return SherpaOnnxCreateOfflineStream(recognizer_);
    } catch (const std::exception& e) {
        FCITX_ERROR() << "[voice-input:sherpa_offline] CreateStream exception: " << e.what();
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

void SherpaOfflineRecognizerHolder::DestroyStream(
    const SherpaOnnxOfflineStream* stream) const {
    if (stream) {
        try {
            SherpaOnnxDestroyOfflineStream(stream);
        } catch (...) {}
    }
}

std::string SherpaOfflineRecognizerHolder::Transcribe(
    const float* pcm, size_t frames, const std::atomic<bool>& cancelled) {
    if (!recognizer_ || !pcm || frames == 0) return "";

    const SherpaOnnxOfflineStream* stream = nullptr;
    const SherpaOnnxOfflineRecognizerResult* result = nullptr;
    std::string text;

    // 解码过程不持有锁（单次调用较长），但 recognizer_ 由 holder 共享且
    // 引擎在会话全部结束前不会被销毁，解码期间 holder 的成员不会变更。
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
        FCITX_ERROR() << "[voice-input:sherpa_offline] Transcribe exception: " << e.what();
    } catch (...) {
        FCITX_ERROR() << "[voice-input:sherpa_offline] Transcribe unknown exception";
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
    if (!pcm || frames == 0 || state_->cancelled.load(std::memory_order_relaxed) ||
        state_->finished.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard<std::mutex> lock(bufferMutex_);
    pcmBuffer_.insert(pcmBuffer_.end(), pcm, pcm + frames);
}

void SherpaOfflineAsrSession::End() {
    state_->finished.store(true, std::memory_order_release);
    std::vector<float> buffer;
    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        if (pcmBuffer_.empty()) return;
        buffer.swap(pcmBuffer_);
    }

    // 用 shared_from_this 保活会话：worker 线程可能因解码较慢在
    // JoinWithTimeout 超时后被 detach，此时会话对象仍必须存活。
    auto self = std::static_pointer_cast<SherpaOfflineAsrSession>(shared_from_this());
    state_->workerDone.store(false, std::memory_order_release);
    workerThread_ = std::make_unique<std::thread>(
        [self, buf = std::move(buffer)]() mutable {
            self->TranscribeWorker(std::move(buf));
            self->state_->workerDone.store(true, std::memory_order_release);
        });
}

void SherpaOfflineAsrSession::Cancel() {
    state_->cancelled.store(true, std::memory_order_release);
}

void SherpaOfflineAsrSession::JoinWithTimeout(std::chrono::milliseconds timeout) {
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
    // 离线解码在 End() 时启动（与 OpenAI 批处理一致），此处无操作。
}

void SherpaOfflineAsrSession::TranscribeWorker(std::vector<float> pcm) {
    auto state = state_;
    auto cb = resultCb_;
    auto ecb = errorCb_;

    if (!holder_) {
        if (ecb) ecb("SherpaOffline recognizer is null");
        if (cb) cb("", true, state->sessionId);
        return;
    }

    std::string text = holder_->Transcribe(pcm.data(), pcm.size(), state->cancelled);

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

    FCITX_DEBUG() << "[voice-input:sherpa_offline] final session=" << state->sessionId
                  << " \"" << text << "\"";
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
    ResolvedPaths paths = ResolveSherpaOfflineModelPaths(config);
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

    // X-ASR 为 BPE 建模单元，但 greedy_search 解码仅依赖 tokens.txt 直接映射，
    // 无需 bpe_vocab。切勿传入 bpe.model：Ssentencepiece::LoadVocab 期望
    // "token score" 文本格式，传二进制 proto 会直接 exit(-1) 终止进程。
    c_cfg.decoding_method = "greedy_search";
    c_cfg.max_active_paths = 8;

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
    FCITX_INFO() << "[voice-input:sherpa_offline] Initialized successfully. encoder=" << paths.encoder
                 << " decoder=" << paths.decoder << " joiner=" << paths.joiner
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
    auto session = std::make_shared<SherpaOfflineAsrSession>(holder_, errorCb_, sid);
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
