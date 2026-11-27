#pragma once

#include <string>

namespace fcitx {

/// 解析热词文件路径。
///
/// configured 非空时只接受该路径（不存在或不可读则告警并返回空，不回退到默认
/// 位置）；为空时依次探测 $XDG_DATA_HOME/fcitx5/voice-input/hotwords.txt 与
/// $HOME/.config/fcitx5/hotwords.txt。返回空串表示未配置热词。
///
/// 存在性之外还要求文件可真正打开：把不可读路径交给 sherpa 会让它
/// `_Exit(-1)` 终止宿主进程（issue #82）。
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

/// bpe 建模单元只使用模型的 tokens.txt，忽略未验证的额外词表。
std::string ResolveSherpaBpeVocab(const std::string& tokensPath);

/// 按编码方式准备热词文件。
///
/// 前置校验对所有编码一致：路径必须是可读且含内容的普通文件。sherpa 在
/// 读不到热词文件时以 `_Exit(-1)` 终止宿主进程，故不可读/为空一律返回空串，
/// 由调用方退回 greedy_search。
///
/// BpePrefixed 下把每个汉字拆成空格分隔的独立词（保留行尾 ":score"），
/// 生成文件写入用户缓存目录并返回其路径；其余情况原样返回。
/// 生成失败返回空串，调用方应关闭热词。
std::string PrepareSherpaHotwordsFile(const std::string& hotwordsPath,
                                      HotwordEncoding encoding,
                                      const char* logTag);

} // namespace fcitx
