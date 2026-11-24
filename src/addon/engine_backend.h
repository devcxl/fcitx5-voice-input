#pragma once

// ASR 后端的构造与热切换。
//
// 本文件把三件事从 engine.cpp 中拆出来：
//   1. 后端配置的**值快照**（AsrBackendSnapshot），后台线程只读它；
//   2. 按快照构造并 Init 引擎（CreateAsrEngine），可在任意线程调用；
//   3. 本地模型的后台准备器（AsrPrepareCoordinator）与 GUI 可查询的只读状态。

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <fcitx-config/configuration.h>
#include <fcitx-utils/i18n.h>

#include "asr/asr_engine.h"

namespace fcitx {

/// 决定实例化哪个 AsrEngine；不来自用户输入，由后端名映射得到。
enum class AsrEngineKind {
    Openai,
    OpenaiRealtime,
    Volcengine,
    Mistral,
    SherpaOnnx,
    SherpaOffline,
};

/// 一个后端的完整值快照。
///
/// 由主线程从 Fcitx 配置对象填充，之后按值移交给后台线程；后台线程不得再访问
/// VoiceInputEngine 的配置成员（那些成员会被 setConfig/setSubConfig 并发修改）。
struct AsrBackendSnapshot {
    AsrEngineKind kind = AsrEngineKind::Openai;
    /// 配置里的后端名（"openai" / "sherpa_offline" ...），用于日志与状态。
    std::string backend;
    /// 纯值配置：只含字符串/整数，不含任何 Fcitx 对象或回调。
    AsrEngine::Config config;
    /// true 表示需要加载本地模型，Init 可能耗时数百毫秒到数秒。
    bool local = false;
};

/// 该后端是否需要本地模型（sherpa_onnx / sherpa_offline）。
bool IsLocalAsrBackend(const std::string& backend);

/// 按快照构造并 Init 一个后端。线程无关：只读 snapshot，不触碰引擎状态。
/// 失败返回 nullptr（日志已由具体后端记录）。
std::unique_ptr<AsrEngine> CreateAsrEngine(const AsrBackendSnapshot& snapshot);

/// 本地模型的运行状态，通过 getSubConfig("model/status/online|offline") 暴露。
///
/// 这是**只读镜像**：值只由主线程更新，setSubConfig 对这两个 path 一律忽略，
/// 也不会写任何 INI 文件。它沿用 Fcitx 配置通道（Controller GetConfig）是为了
/// 让外部 GUI 能异步轮询，而不引入新的私有协议。
FCITX_CONFIGURATION(ModelRuntimeStatusConfig,
    Option<std::string> state{this, "State", _("本地模型状态（只读）"), "idle"};
    Option<std::string> detail{this, "Detail", _("状态说明（只读）"), ""};
    Option<std::string> model{this, "Model", _("模型标识（只读）"), ""};
    Option<std::string> version{this, "Version", _("模型版本（只读）"), ""};
);

} // namespace fcitx
