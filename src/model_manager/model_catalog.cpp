#include "model_catalog.h"

#include <array>

namespace fcitx::model_manager {

namespace {

// ─── online-zipformer ────────────────────────────────────────────────────────
//
// 上游： sherpa-onnx-streaming-zipformer-zh-xlarge-int8-2025-06-30
// 归档： sha256 30437d84…（GitHub Release API digest 与实际下载逐字节一致）
//
// 逐文件摘要经**双源交叉验证**：
//   (a) 官方 release 归档解包（tar 与 bsdtar 双工具结果一致）；
//   (b) 上游发布方 Hugging Face 仓库的 LFS sha256。
// 两边四个文件全部相符。
constexpr ModelFileSpec kOnlineFiles[] = {
    {"encoder", "encoder.int8.onnx",
     "f2c543a0330e1ed0bd09c82e4ae7d3f1cbee10a15feca638fcc4f88083a36b8a",
     761133737},
    {"decoder", "decoder.onnx",
     "8f9c903da2818f207304a3f30b9eeb30028e30398f333c1e95e12c97704173e6",
     8533022},
    {"joiner", "joiner.int8.onnx",
     "f76ffce14b6ef80098cfdbce8846896ff68133970abc314eafab632f910df0d7",
     1545417},
    {"tokens", "tokens.txt",
     "6722bd1585f46f84456b29c3550a343a3cc375b971645773c02ed8e0b4e2405c",
     18626},
};

// ─── offline-x-asr ───────────────────────────────────────────────────────────
//
// 上游： sherpa-onnx-x-asr-zipformer-transducer-zh-en-int8-2026-06-03
// 归档： sha256 7592d1a1…（GitHub Release API digest 与实际下载逐字节一致）
//
// 逐文件摘要**以官方 release 归档解包结果为准**（tar / bsdtar 双工具一致，
// bzip2 -t 完整性通过）。注意：上游发布方 HF 仓库中该模型的 encoder/decoder
// 摘要与归档内文件不同（joiner/tokens 相同），属另一次导出。
// 安装器的信任锚是"归档 sha256 → 解包产物"，因此这里必须记录归档内实际内容，
// 否则每次安装都会在逐文件校验处失败。**不要**用 HF 的值替换。
constexpr ModelFileSpec kOfflineFiles[] = {
    {"encoder", "encoder-epoch-99-avg-1.int8.onnx",
     "fea6a8d0066b327401b603a7deff24e685379f523771bafd5127aa572018951c",
     161015628},
    {"decoder", "decoder-epoch-99-avg-1.onnx",
     "fd2ac10c5936a1411f7a68e669e8cfec882b17791dd2ca44ab9baf69bb8ab855",
     11309084},
    {"joiner", "joiner-epoch-99-avg-1.int8.onnx",
     "82e88dad29301e29144565cd3b7347fa163f07ddb30ea60346b7c4c536adf4ba",
     2581422},
    {"tokens", "tokens.txt",
     "ad0df596d9cc60c90795409549ccf7d1c4f901bdc060dd6b2b067d9ef2fddfaf",
     59240},
};

constexpr ModelSpec kSpecs[] = {
    {
        "online-zipformer",
        "sherpa-onnx-streaming-zipformer-zh-xlarge-int8-2025-06-30",
        "sherpa-onnx-streaming",
        /*streaming=*/true,
        "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/"
        "sherpa-onnx-streaming-zipformer-zh-xlarge-int8-2025-06-30.tar.bz2",
        "30437d84dc4861740d40166212ab05f3728945443d53691620075c4031fb88e1",
        597755927,
        kOnlineFiles,
        std::size(kOnlineFiles),
    },
    {
        "offline-x-asr",
        "sherpa-onnx-x-asr-zipformer-transducer-zh-en-int8-2026-06-03",
        "sherpa-onnx-offline",
        /*streaming=*/false,
        "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/"
        "sherpa-onnx-x-asr-zipformer-transducer-zh-en-int8-2026-06-03.tar.bz2",
        "7592d1a1e9bc1e034c39fbaab9b0db8af46429d95d64c6e2a69593934af10e9c",
        136902463,
        kOfflineFiles,
        std::size(kOfflineFiles),
    },
};

} // namespace

const ModelSpec* ListModelSpecs(size_t* count) {
    if (count) *count = std::size(kSpecs);
    return kSpecs;
}

const ModelSpec* FindModelSpec(std::string_view id) {
    for (const auto& spec : kSpecs) {
        if (id == spec.id) return &spec;
    }
    return nullptr;
}

const ModelFileSpec* FindModelFileByRole(const ModelSpec& spec,
                                         std::string_view role) {
    for (size_t i = 0; i < spec.fileCount; ++i) {
        if (role == spec.files[i].role) return &spec.files[i];
    }
    return nullptr;
}

const ModelFileSpec* FindModelFile(const ModelSpec& spec,
                                   std::string_view relativePath) {
    for (size_t i = 0; i < spec.fileCount; ++i) {
        if (relativePath == spec.files[i].relativePath) return &spec.files[i];
    }
    return nullptr;
}

} // namespace fcitx::model_manager
