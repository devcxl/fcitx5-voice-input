#pragma once

// 本地 sherpa-onnx 模型的受控路径解析。
//
// 与旧的 ResolveSherpaModelPaths 的根本区别：
//   - 不再扫描目录、不再按文件名前缀猜角色、不再接受用户配置的文件路径；
//   - 每个后端只对应清单里的**一个**固定 id/version，路径由常量拼出；
//   - 返回前已经用内置 SHA-256 逐文件校验，调用方拿到的路径必然可信。
//
// 因此"在线模型喂给离线后端"这类错配在进入 sherpa 之前就被拒绝，
// 而不是等到 sherpa 的 _Exit(-1) 或 SIGSEGV。

#include <string>

#include "model_catalog.h"
#include "sherpa_model_verify.h"

namespace fcitx {

/// 本地 sherpa 后端的种类。与清单的 streaming 字段对应，不来自用户输入。
enum class SherpaBackendKind {
    /// 流式识别：SherpaOnnxAsrEngine（OnlineRecognizer）。
    Streaming,
    /// 非流式整段识别：SherpaOfflineAsrEngine（OfflineRecognizer）。
    Offline,
};

/// 解析结果：既报告状态，也在成功时给出可直接喂给 sherpa 的绝对路径。
struct SherpaResolvedModel {
    SherpaModelStatus status = SherpaModelStatus::NotInstalled;
    /// 命中的清单 id/version（诊断用；失败时可能为空）。
    std::string modelId;
    std::string version;
    /// 版本目录绝对路径（成功时非空）。
    std::string versionDir;
    /// 逐文件绝对路径（成功时非空）。
    std::string encoder;
    std::string decoder;
    std::string joiner;
    std::string tokens;
    /// 失败时清单内的 relativePath 或诊断片段，不含用户可控路径。
    std::string failedRelativePath;

    bool ok() const { return status == SherpaModelStatus::Ok; }
};

/// 返回该后端对应的清单项；无对应项时返回 nullptr。
const model_manager::ModelSpec* FindSpecForKind(SherpaBackendKind kind);

/// 解析并校验该后端的受控模型。
///
/// 步骤：kind → 清单项 → VersionDir() → 逐文件 SHA-256。
/// 任一步失败都返回非 Ok 状态且不填充路径，调用方必须据此关闭本地识别，
/// 不得回退到目录扫描或用户指定的路径。
///
/// 这里是**唯一**的模型路径来源：不接受目录参数、清单 id 或版本覆盖，
/// 因此不存在「把任意目录当成模型目录」的入口。
SherpaResolvedModel ResolveControlledSherpaModel(SherpaBackendKind kind);

} // namespace fcitx
