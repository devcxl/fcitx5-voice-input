#pragma once

#include <string>

namespace fcitx {

/// 本地 sherpa-onnx 模型文件路径。
struct SherpaModelPaths {
    std::string encoder;
    std::string decoder;
    std::string joiner;
    std::string tokens;
    bool valid = false;
};

/// 用户显式配置的模型文件路径，留空表示自动探测。
struct SherpaModelPathOverrides {
    std::string encoder;
    std::string decoder;
    std::string joiner;
    std::string tokens;
};

/// 探测 sherpa-onnx transducer 模型（encoder/decoder/joiner/tokens.txt）。
///
/// 查找顺序：
///   1. overrides 四项齐全且文件存在；
///   2. modelDir（配置项）；
///   3. $XDG_DATA_HOME/fcitx5/voice-input/models（无 XDG_DATA_HOME 时用
///      $HOME/.local/share）；
///   4. /usr/local/share/fcitx5/voice-input/models。
/// 每个根目录先检查自身，再检查其一级子目录；encoder/decoder/joiner 按文件名前缀匹配
/// 并优先 int8 量化文件，tokens 固定为 tokens.txt。
///
/// logTag 用作日志前缀，例如 "voice-input:sherpa_onnx"。
SherpaModelPaths ResolveSherpaModelPaths(
    const std::string& modelDir, const SherpaModelPathOverrides& overrides,
    const char* logTag);

/// 解析热词文件路径。
///
/// configured 非空时只接受该路径（不存在则告警并返回空，不回退到默认位置）；
/// 为空时依次探测 $XDG_DATA_HOME/fcitx5/voice-input/hotwords.txt 与
/// $HOME/.config/fcitx5/hotwords.txt。返回空串表示未配置热词。
std::string ResolveSherpaHotwordsFile(const std::string& configured,
                                      const char* logTag);

/// 热词编码方式，由模型词表形态决定。
enum class HotwordEncoding {
    /// 词表含裸汉字 token（常规中文/cjkchar 模型）：modeling_unit=cjkchar，热词原样。
    CjkChar,
    /// 词表只有 "▁汉字"（如 X-ASR）：modeling_unit=bpe，且热词需把汉字逐字拆开。
    BpePrefixed,
    /// 无汉字 token（英文 bpe 模型）：modeling_unit=bpe，热词原样。
    Bpe,
};

/// 扫描 tokens.txt 判定热词编码方式；无法判定时按 Bpe 处理。
HotwordEncoding DetectHotwordEncoding(const std::string& tokensPath);

/// bpe 建模单元使用的词表：模型目录内有 bpe.vocab 时优先它，
/// 否则退化为 tokens.txt（token id 被当作 score，切分仍可用）。
std::string ResolveSherpaBpeVocab(const std::string& tokensPath);

/// 按编码方式准备热词文件。
///
/// BpePrefixed 下把每个汉字拆成空格分隔的独立词（保留行尾 ":score"），
/// 生成文件写入用户缓存目录并返回其路径；其余情况原样返回。
/// 生成失败返回空串，调用方应关闭热词。
std::string PrepareSherpaHotwordsFile(const std::string& hotwordsPath,
                                      HotwordEncoding encoding,
                                      const char* logTag);

} // namespace fcitx
