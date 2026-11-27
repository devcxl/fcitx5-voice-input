#include "sherpa_paths.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <vector>

#include <fcitx-utils/log.h>

#include "model_paths.h"

namespace fcitx {

namespace {

namespace fsys = std::filesystem;

bool IsFile(const std::string& path) {
    std::error_code ec;
    return fsys::is_regular_file(path, ec);
}

} // namespace

std::string ResolveSherpaHotwordsFile(const std::string& configured,
                                      const char* logTag) {
    if (!configured.empty()) {
        if (IsFile(configured) && std::ifstream(configured)) return configured;
        FCITX_WARN() << "[" << logTag
                     << "] 配置的热词文件不存在或不可读取，已忽略: " << configured;
        return {};
    }

    // 与受控模型共用同一份 XDG 规则（相对 XDG_DATA_HOME 按 spec 忽略）。
    const std::string userShare = model_manager::DataHomeDir();
    if (!userShare.empty()) {
        const fsys::path candidate =
            fsys::path(userShare) / "fcitx5/voice-input/hotwords.txt";
        if (IsFile(candidate.string()) && std::ifstream(candidate)) {
            return candidate.string();
        }
    }

    if (const char* home = std::getenv("HOME"); home && *home) {
        const fsys::path candidate = fsys::path(home) / ".config/fcitx5/hotwords.txt";
        if (IsFile(candidate.string()) && std::ifstream(candidate)) {
            return candidate.string();
        }
    }

    return {};
}


namespace {

bool DecodeUtf8(const std::string& s, size_t i, uint32_t* cp, size_t* len) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) {
        *cp = b0;
        *len = 1;
        return true;
    }
    size_t n = 0;
    uint32_t v = 0;
    if ((b0 & 0xE0) == 0xC0) {
        n = 2;
        v = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        n = 3;
        v = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        n = 4;
        v = b0 & 0x07;
    } else {
        *cp = b0;
        *len = 1;
        return false;
    }
    if (i + n > s.size()) {
        *cp = b0;
        *len = 1;
        return false;
    }
    for (size_t k = 1; k < n; ++k) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0) != 0x80) {
            *cp = b0;
            *len = 1;
            return false;
        }
        v = (v << 6) | (b & 0x3F);
    }
    *cp = v;
    *len = n;
    return true;
}

bool IsHan(uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x2FA1F);
}

std::string FirstField(const std::string& line) {
    const auto pos = line.find_first_of(" \t");
    return pos == std::string::npos ? line : line.substr(0, pos);
}

bool IsSingleHanToken(const std::string& token, bool prefixed) {
    size_t i = 0;
    if (prefixed) {
        static constexpr char kPrefix[] = "\xE2\x96\x81"; // ▁
        if (token.size() < 4 || token.compare(0, 3, kPrefix) != 0) return false;
        i = 3;
    }
    if (i >= token.size()) return false;
    uint32_t cp = 0;
    size_t len = 0;
    if (!DecodeUtf8(token, i, &cp, &len)) return false;
    return i + len == token.size() && IsHan(cp);
}

/// 汉字逐字拆词；保留行尾 ":score"；空行返回空串。
std::string NormalizeHotwordLine(const std::string& line) {
    std::string body = line;
    std::string score;

    const auto lastSpace = body.find_last_of(" \t");
    const std::string lastToken =
        lastSpace == std::string::npos ? body : body.substr(lastSpace + 1);
    if (!lastToken.empty() && lastToken[0] == ':') {
        score = lastToken;
        body = lastSpace == std::string::npos ? std::string{}
                                              : body.substr(0, lastSpace);
    }

    std::vector<std::string> words;
    std::string asciiRun;

    auto flushAscii = [&]() {
        if (!asciiRun.empty()) {
            words.push_back(asciiRun);
            asciiRun.clear();
        }
    };

    for (size_t i = 0; i < body.size();) {
        const auto b = static_cast<unsigned char>(body[i]);
        if (b == ' ' || b == '\t' || b == '\r' || b == '\n') {
            flushAscii();
            ++i;
            continue;
        }
        uint32_t cp = 0;
        size_t len = 1;
        if (!DecodeUtf8(body, i, &cp, &len)) {
            asciiRun.push_back(body[i]);
            ++i;
            continue;
        }
        if (IsHan(cp)) {
            flushAscii();
            words.emplace_back(body.substr(i, len));
        } else {
            asciiRun.append(body.substr(i, len));
        }
        i += len;
    }
    flushAscii();

    if (words.empty()) return {};

    std::string out;
    for (size_t k = 0; k < words.size(); ++k) {
        if (k) out.push_back(' ');
        out += words[k];
    }
    if (!score.empty()) {
        out.push_back(' ');
        out += score;
    }
    return out;
}

std::string UserCacheDir() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) return xdg;
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.cache";
    }
    return {};
}

} // namespace

HotwordEncoding DetectHotwordEncoding(const std::string& tokensPath) {
    std::ifstream in(tokensPath);
    if (!in) return HotwordEncoding::Bpe;

    bool sawBareHan = false;
    bool sawPrefixedHan = false;
    std::string line;
    while (std::getline(in, line)) {
        const std::string token = FirstField(line);
        if (token.empty()) continue;
        if (IsSingleHanToken(token, /*prefixed=*/false)) {
            sawBareHan = true;
            break;
        }
        if (!sawPrefixedHan && IsSingleHanToken(token, /*prefixed=*/true)) {
            sawPrefixedHan = true;
        }
    }

    if (sawBareHan) return HotwordEncoding::CjkChar;
    if (sawPrefixedHan) return HotwordEncoding::BpePrefixed;
    return HotwordEncoding::Bpe;
}

std::string ResolveSherpaBpeVocab(const std::string& tokensPath) {
    // 仅使用经过模型清单校验的词表，不加载目录中未经校验的 bpe.vocab。
    // tokens.txt 的 "token id" 会被当作 score 读取；逐字/整词切分仍然可用。
    return tokensPath;
}

std::string PrepareSherpaHotwordsFile(const std::string& hotwordsPath,
                                      HotwordEncoding encoding,
                                      const char* logTag) {
    // 交给 sherpa 的热词路径必须是可读且非空的普通文件：sherpa 的
    // InitHotwords() 在 OpenInputFile 失败时走 SHERPA_ONNX_EXIT(-1)，
    // 即 _Exit 直接终止宿主 fcitx5 进程（issue #82），try/catch 拦不住。
    // 三种编码共用同一道前置校验，失败一律返回空串让调用方退回
    // greedy_search；不能只在 BpePrefixed 分支里检查。
    if (!IsFile(hotwordsPath)) {
        FCITX_WARN() << "[" << logTag << "] 热词文件不存在，已关闭热词: "
                     << hotwordsPath;
        return {};
    }

    std::ifstream in(hotwordsPath);
    if (!in) {
        FCITX_WARN() << "[" << logTag << "] 无法读取热词文件: " << hotwordsPath;
        return {};
    }

    std::string line;

    if (encoding != HotwordEncoding::BpePrefixed) {
        // 其余编码原样交给 sherpa，但仍要求文件含至少一条非空白行：空热词表
        // 只会让 sherpa 白建一个用不上的 modified_beam_search 上下文图。
        size_t nonEmptyLines = 0;
        while (std::getline(in, line)) {
            if (line.find_first_not_of(" \t\r\n") != std::string::npos) {
                ++nonEmptyLines;
            }
        }
        if (nonEmptyLines == 0) {
            FCITX_WARN() << "[" << logTag << "] 热词文件为空，已关闭热词: "
                         << hotwordsPath;
            return {};
        }
        return hotwordsPath;
    }

    std::string normalized;
    size_t lines = 0;
    while (std::getline(in, line)) {
        const std::string one = NormalizeHotwordLine(line);
        if (one.empty()) continue;
        normalized += one;
        normalized.push_back('\n');
        ++lines;
    }
    if (lines == 0) {
        FCITX_WARN() << "[" << logTag << "] 热词文件为空，已关闭热词: "
                     << hotwordsPath;
        return {};
    }

    const std::string cacheDir = UserCacheDir();
    if (cacheDir.empty()) {
        FCITX_WARN() << "[" << logTag << "] 无法确定缓存目录，已关闭热词";
        return {};
    }

    std::error_code ec;
    const fsys::path dir = fsys::path(cacheDir) / "fcitx5/voice-input";
    fsys::create_directories(dir, ec);
    if (ec) {
        FCITX_WARN() << "[" << logTag << "] 无法创建缓存目录 " << dir.string()
                     << ": " << ec.message();
        return {};
    }

    const fsys::path target = dir / "hotwords-bpe.txt";
    const fsys::path temp = dir / "hotwords-bpe.txt.tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) {
            FCITX_WARN() << "[" << logTag << "] 无法写入 " << temp.string();
            return {};
        }
        out << normalized;
        if (!out) {
            FCITX_WARN() << "[" << logTag << "] 写入热词文件失败: " << temp.string();
            return {};
        }
    }
    ::chmod(temp.c_str(), S_IRUSR | S_IWUSR);
    fsys::rename(temp, target, ec);
    if (ec) {
        fsys::remove(temp, ec);
        FCITX_WARN() << "[" << logTag << "] 无法生成热词文件 " << target.string()
                     << ": " << ec.message();
        return {};
    }
    ::chmod(target.c_str(), S_IRUSR | S_IWUSR);

    FCITX_INFO() << "[" << logTag << "] 热词按汉字逐字拆分: " << target.string()
                 << " lines=" << lines;
    return target.string();
}

} // namespace fcitx

