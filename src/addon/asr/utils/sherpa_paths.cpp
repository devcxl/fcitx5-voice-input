#include "sherpa_paths.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <vector>

#include <fcitx-utils/log.h>

namespace fcitx {

namespace {

namespace fsys = std::filesystem;

bool IsFile(const std::string& path) {
    std::error_code ec;
    return fsys::is_regular_file(path, ec);
}

/// 目录内按前缀匹配 *.onnx，优先 int8 量化文件。
std::string FindMatchingModelFile(const fsys::path& dir,
                                  const std::string& prefix) {
    std::string int8Candidate;
    std::string fp32Candidate;

    std::error_code ec;
    for (const auto& entry : fsys::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        const std::string filename = entry.path().filename().string();
        if (filename.rfind(prefix, 0) != 0) continue;
        if (filename.rfind(".onnx") == std::string::npos) continue;

        if (filename.find(".int8.onnx") != std::string::npos) {
            int8Candidate = entry.path().string();
        } else {
            fp32Candidate = entry.path().string();
        }
    }
    return !int8Candidate.empty() ? int8Candidate : fp32Candidate;
}

/// 显式配置的文件必须存在；未配置的按目录前缀探测。
std::string ResolveFile(const fsys::path& dir, const std::string& configured,
                        const std::string& prefix) {
    if (!configured.empty()) {
        return IsFile(configured) ? configured : std::string{};
    }
    return FindMatchingModelFile(dir, prefix);
}

bool CheckModelDir(const fsys::path& dir,
                   const SherpaModelPathOverrides& overrides,
                   SherpaModelPaths* out) {
    std::error_code ec;
    if (!fsys::is_directory(dir, ec)) return false;

    const std::string encoder =
        ResolveFile(dir, overrides.encoder, "encoder");
    const std::string decoder =
        ResolveFile(dir, overrides.decoder, "decoder");
    const std::string joiner = ResolveFile(dir, overrides.joiner, "joiner");
    // tokens 固定为 tokens.txt，不参与前缀探测
    std::string tokens;
    if (!overrides.tokens.empty()) {
        tokens = IsFile(overrides.tokens) ? overrides.tokens : std::string{};
    } else {
        const fsys::path candidate = dir / "tokens.txt";
        tokens = IsFile(candidate.string()) ? candidate.string() : std::string{};
    }

    if (encoder.empty() || decoder.empty() || joiner.empty() || tokens.empty()) {
        return false;
    }

    out->encoder = encoder;
    out->decoder = decoder;
    out->joiner = joiner;
    out->tokens = tokens;
    out->valid = true;
    return true;
}

std::string UserDataDir() {
    const char* xdgData = std::getenv("XDG_DATA_HOME");
    if (xdgData && *xdgData) return xdgData;

    const char* home = std::getenv("HOME");
    if (home && *home) return std::string(home) + "/.local/share";
    return {};
}

} // namespace

SherpaModelPaths ResolveSherpaModelPaths(
    const std::string& modelDir, const SherpaModelPathOverrides& overrides,
    const char* logTag) {
    SherpaModelPaths result;

    if (!overrides.encoder.empty() && !overrides.decoder.empty() &&
        !overrides.joiner.empty() && !overrides.tokens.empty()) {
        if (IsFile(overrides.encoder) && IsFile(overrides.decoder) &&
            IsFile(overrides.joiner) && IsFile(overrides.tokens)) {
            result.encoder = overrides.encoder;
            result.decoder = overrides.decoder;
            result.joiner = overrides.joiner;
            result.tokens = overrides.tokens;
            result.valid = true;
            return result;
        }
        FCITX_WARN() << "[" << logTag
                     << "] 显式配置的模型文件不完整，回退到目录探测";
    }

    std::vector<fsys::path> bases;
    if (!modelDir.empty()) bases.emplace_back(modelDir);

    const std::string userShare = UserDataDir();
    if (!userShare.empty()) {
        bases.emplace_back(fsys::path(userShare) / "fcitx5/voice-input/models");
    }
    bases.emplace_back("/usr/local/share/fcitx5/voice-input/models");

    for (const auto& base : bases) {
        if (CheckModelDir(base, overrides, &result)) {
            FCITX_INFO() << "[" << logTag << "] Found valid model directory: "
                         << base.string();
            return result;
        }

        std::error_code ec;
        for (const auto& entry : fsys::directory_iterator(base, ec)) {
            if (ec) break;
            if (!entry.is_directory(ec)) continue;
            if (CheckModelDir(entry.path(), overrides, &result)) {
                FCITX_INFO() << "[" << logTag
                             << "] Found valid model directory: "
                             << entry.path().string();
                return result;
            }
        }
    }

    return result;
}

std::string ResolveSherpaHotwordsFile(const std::string& configured,
                                      const char* logTag) {
    if (!configured.empty()) {
        if (IsFile(configured)) return configured;
        FCITX_WARN() << "[" << logTag
                     << "] 配置的热词文件不存在，已忽略: " << configured;
        return {};
    }

    const std::string userShare = UserDataDir();
    if (!userShare.empty()) {
        const fsys::path candidate =
            fsys::path(userShare) / "fcitx5/voice-input/hotwords.txt";
        if (IsFile(candidate.string())) return candidate.string();
    }

    if (const char* home = std::getenv("HOME"); home && *home) {
        const fsys::path candidate = fsys::path(home) / ".config/fcitx5/hotwords.txt";
        if (IsFile(candidate.string())) return candidate.string();
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
    const fsys::path dir = fsys::path(tokensPath).parent_path();
    const fsys::path candidate = dir / "bpe.vocab";
    if (IsFile(candidate.string())) return candidate.string();
    // tokens.txt 的 "token id" 会被当作 score 读取；逐字/整词切分仍然可用。
    return tokensPath;
}

std::string PrepareSherpaHotwordsFile(const std::string& hotwordsPath,
                                      HotwordEncoding encoding,
                                      const char* logTag) {
    if (encoding != HotwordEncoding::BpePrefixed) return hotwordsPath;

    std::ifstream in(hotwordsPath);
    if (!in) {
        FCITX_WARN() << "[" << logTag << "] 无法读取热词文件: " << hotwordsPath;
        return {};
    }

    std::string normalized;
    std::string line;
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

