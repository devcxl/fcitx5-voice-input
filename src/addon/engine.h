#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/event.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/instance.h>

#include "config/voiceinput-config.h"
#include "asr/asr_engine.h"
#include "engine_asr_prepare.h"
#include "engine_backend.h"
#include "pipeline/pipeline.h"
#include "types.h"

namespace fcitx {

class VoiceInputEngine : public InputMethodEngineV2 {
public:
    VoiceInputEngine(Instance* instance);
    ~VoiceInputEngine() override;

    void activate(const InputMethodEntry& entry,
                  InputContextEvent& event) override;

    void deactivate(const InputMethodEntry& entry,
                    InputContextEvent& event) override;

    void keyEvent(const InputMethodEntry& entry, KeyEvent& keyEvent) override;

    std::vector<InputMethodEntry> listInputMethods() override;

    const Configuration* getConfig() const override { return &config_; }
    void setConfig(const RawConfig& rawConfig) override;
    void reloadConfig() override;

    const Configuration* getSubConfig(const std::string& path) const override;
    void setSubConfig(const std::string& path,
                      const RawConfig& rawConfig) override;

    std::string subModeLabelImpl(const InputMethodEntry& entry,
                                 InputContext& ic) override;

private:
    void InitializeIfNeeded();
    void OnAsrResult(const std::string& text);
    void OnVadStatus(bool speaking);
    void PollResults();
    void ClearUI();
    void DeactivateContext(bool keepCapture);
    void SetStatus(const std::string& text);

    // 后端构造与切换（engine_backend.cpp）。全部只在主线程调用。
    AsrBackendSnapshot SnapshotActiveAsrBackend() const;
    void ReloadAsrBackend(const char* reason);
    void ApplyPreparedAsrBackend(uint64_t generation,
                                 std::unique_ptr<AsrEngine> engine,
                                 const std::string& error);
    void DropAsrEngine();
    void RotateAsrGeneration();
    void UpdateModelStatus(const std::string& backend, const char* state,
                           const std::string& detail);
    void UpdateModelIdentity(const std::string& backend,
                             const AsrModelIdentity& identity);
    /// 不再被选中的本地后端不能继续显示 ready/loading（会误报）。
    void ResetInactiveLocalStatuses(const std::string& activeBackend);

    Instance* instance_;
    std::unique_ptr<Pipeline> pipeline_;
    EventDispatcher eventDispatcher_;
    std::vector<std::unique_ptr<HandlerTableEntry<EventHandler>>> eventWatchers_;
    VoiceInputConfig config_;
    OpenAIAsrConfig openaiConfig_;
    VolcengineAsrConfig volcengineConfig_;
    MistralAsrConfig mistralConfig_;
    SherpaOnnxAsrConfig sherpaOnnxConfig_;
    SherpaOfflineAsrConfig sherpaOfflineConfig_;

    // 本地模型的异步准备。工作线程从不引用本对象（见 engine_backend.h）。
    std::unique_ptr<AsrPrepareCoordinator> asrPreparer_;
    // 主线程独占：当前请求代次 / 已装入 pipeline 的后端名（空表示无引擎）。
    uint64_t asrPrepareGeneration_ = 0;
    std::string installedBackend_;
    // getSubConfig("model/status/online|offline") 的只读镜像，仅主线程写入。
    ModelRuntimeStatusConfig onlineModelStatus_;
    ModelRuntimeStatusConfig offlineModelStatus_;

    // Main-thread only: generation checks do not keep an InputContext alive.
    TrackableObjectReference<InputContext> activeIc_;
    // Survives FocusOut: only the context that opened capture may close it.
    TrackableObjectReference<InputContext> captureOwnerIc_;
    std::atomic<uint64_t> activeGeneration_{0};
    std::atomic<uint64_t> sessionGeneration_{0};
    bool initialized_ = false;

    std::string statusText_;
    std::string pendingPreeditText_;
    uint64_t pendingPreeditUtteranceId_ = 0;
};

} // namespace fcitx
