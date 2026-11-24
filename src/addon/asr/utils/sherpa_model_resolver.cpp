#include "sherpa_model_resolver.h"

#include <string_view>

#include "model_paths.h"

namespace fcitx {

namespace {

// 后端种类到清单 id 的唯一映射。清单里是 constexpr 常量，这里只按 id 查，
// 不复制任何版本号、文件名或摘要。
constexpr const char* kStreamingId = "online-zipformer";
constexpr const char* kOfflineId = "offline-x-asr";

const model_manager::ModelSpec* LookupSpec(std::string_view id, bool streaming) {
    const model_manager::ModelSpec* spec =
        model_manager::FindModelSpec(id);
    if (!spec) return nullptr;
    // 清单自报的 streaming 必须与后端种类一致；不一致说明清单被改坏，
    // 不能拿它去配 recognizer（会走向 sherpa 的致命退出路径）。
    if (spec->streaming != streaming) return nullptr;
    return spec;
}

/// 校验清单项在 versionDir 内的全部文件并回填路径。
/// modelsRoot/versionDir 都由清单常量拼出，不接受外部输入。
SherpaResolvedModel ResolveInDir(const model_manager::ModelSpec& spec,
                                 const std::string& modelsRoot,
                                 const std::string& versionDir) {
    SherpaResolvedModel result;
    result.modelId = spec.id;
    result.version = spec.version;
    result.versionDir = versionDir;

    const SherpaModelVerification verification =
        VerifyModelVersionDir(spec, modelsRoot, versionDir);
    result.status = verification.status;
    result.failedRelativePath = verification.failedRelativePath;
    if (!verification.ok()) return result;

    result.encoder = verification.encoder;
    result.decoder = verification.decoder;
    result.joiner = verification.joiner;
    result.tokens = verification.tokens;
    return result;
}

} // namespace

const model_manager::ModelSpec* FindSpecForKind(SherpaBackendKind kind) {
    switch (kind) {
    case SherpaBackendKind::Streaming:
        return LookupSpec(kStreamingId, /*streaming=*/true);
    case SherpaBackendKind::Offline:
        return LookupSpec(kOfflineId, /*streaming=*/false);
    }
    return nullptr;
}

SherpaResolvedModel ResolveControlledSherpaModel(SherpaBackendKind kind) {
    SherpaResolvedModel result;

    const model_manager::ModelSpec* spec = FindSpecForKind(kind);
    if (!spec) {
        result.status = SherpaModelStatus::InvalidSpec;
        return result;
    }

    // 路径只由清单常量拼出；空串表示数据目录不可用或片段非法。
    const std::string modelsRoot = model_manager::DataRootDir();
    const std::string versionDir =
        model_manager::VersionDir(spec->id, spec->version);
    if (modelsRoot.empty() || versionDir.empty()) {
        result.status = SherpaModelStatus::DataDirUnavailable;
        result.modelId = spec->id;
        result.version = spec->version;
        return result;
    }

    return ResolveInDir(*spec, modelsRoot, versionDir);
}

} // namespace fcitx
