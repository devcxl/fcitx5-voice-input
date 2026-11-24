// sherpa_paths 单元测试：热词文件解析与编码判定。
// 受控模型路径/校验的用例在 sherpa_model_verify_test.cpp。
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "asr/utils/sherpa_paths.h"
#include "model_paths.h"

namespace {

namespace fs = std::filesystem;

int failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

void WriteBytes(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("voice-input-paths-test-" + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

/// 保存并恢复环境变量，避免用例之间互相污染。
class EnvGuard {
public:
    explicit EnvGuard(std::initializer_list<const char*> names) {
        for (const char* name : names) {
            const char* value = std::getenv(name);
            saved_.emplace_back(name, value ? std::optional<std::string>(value)
                                            : std::nullopt);
        }
    }
    ~EnvGuard() {
        for (const auto& [name, value] : saved_) {
            if (value) {
                ::setenv(name.c_str(), value->c_str(), 1);
            } else {
                ::unsetenv(name.c_str());
            }
        }
    }

private:
    std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

void TestHotwordsFile() {
    TempDir tmp;
    const fs::path hotwords = tmp.path / "hotwords.txt";
    WriteBytes(hotwords, "x");
    Check(fcitx::ResolveSherpaHotwordsFile(hotwords.string(), "test") ==
              hotwords.string(),
          "hotwords: configured file should be returned");
    Check(fcitx::ResolveSherpaHotwordsFile(
              (tmp.path / "missing.txt").string(), "test")
              .empty(),
          "hotwords: missing configured file should be ignored");
}

void TestHotwordsDefaultLocationFollowsXdg() {
    TempDir tmp;
    EnvGuard guard({"XDG_DATA_HOME", "HOME"});

    const fs::path dataShare = tmp.path / "data";
    const fs::path hotwords = dataShare / "fcitx5/voice-input/hotwords.txt";
    WriteBytes(hotwords, "x");
    ::setenv("XDG_DATA_HOME", dataShare.c_str(), 1);
    ::setenv("HOME", (tmp.path / "home").c_str(), 1);
    Check(fcitx::ResolveSherpaHotwordsFile("", "test") == hotwords.string(),
          "hotwords: default lookup should use absolute XDG_DATA_HOME");

    // 相对 XDG_DATA_HOME 按 XDG spec 忽略，回退 $HOME/.local/share。
    const fs::path homeHotwords =
        tmp.path / "home/.local/share/fcitx5/voice-input/hotwords.txt";
    WriteBytes(homeHotwords, "x");
    ::setenv("XDG_DATA_HOME", "relative/data", 1);
    Check(fcitx::ResolveSherpaHotwordsFile("", "test") ==
              homeHotwords.string(),
          "hotwords: relative XDG_DATA_HOME should fall back to HOME");
}

void TestHotwordEncodingDetection() {
    TempDir tmp;
    const fs::path prefixed = tmp.path / "tokens-prefixed.txt";
    WriteBytes(prefixed, "<blk> 0\n<sos/eos> 1\n▁编 2\n▁程 3\n▁FRONT 4\n");
    Check(fcitx::DetectHotwordEncoding(prefixed.string()) ==
              fcitx::HotwordEncoding::BpePrefixed,
          "encoding: ▁-prefixed Han tokens should select bpe(per-char)");

    const fs::path bare = tmp.path / "tokens-bare.txt";
    WriteBytes(bare, "<blk> 0\n编 2\n程 3\n");
    Check(fcitx::DetectHotwordEncoding(bare.string()) ==
              fcitx::HotwordEncoding::CjkChar,
          "encoding: bare Han tokens should select cjkchar");

    const fs::path english = tmp.path / "tokens-en.txt";
    WriteBytes(english, "<blk> 0\n▁THE 2\nED 3\n");
    Check(fcitx::DetectHotwordEncoding(english.string()) ==
              fcitx::HotwordEncoding::Bpe,
          "encoding: no Han tokens should select bpe");
}

void TestHotwordNormalization() {
    TempDir tmp;
    const fs::path hotwords = tmp.path / "hotwords.txt";
    WriteBytes(hotwords, "编程\nFRONT\n深度学习 :3.5\n\n  \n");
    const fs::path cache = tmp.path / "cache";
    ::setenv("XDG_CACHE_HOME", cache.c_str(), 1);

    const std::string prepared = fcitx::PrepareSherpaHotwordsFile(
        hotwords.string(), fcitx::HotwordEncoding::BpePrefixed, "test");
    Check(prepared == (cache / "fcitx5/voice-input/hotwords-bpe.txt").string(),
          "normalize: derived file should land in the cache dir");

    std::ifstream in(prepared);
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    Check(content == "编 程\nFRONT\n深 度 学 习 :3.5\n",
          "normalize: Han chars should be split, score kept: got \"" +
              content + "\"");
    Check(fcitx::PrepareSherpaHotwordsFile(
              hotwords.string(), fcitx::HotwordEncoding::CjkChar, "test") ==
              hotwords.string(),
          "normalize: cjkchar encoding should keep the original file");
    ::unsetenv("XDG_CACHE_HOME");
}

void TestBpeVocabResolution() {
    TempDir tmp;
    const fs::path tokens = tmp.path / "tokens.txt";
    WriteBytes(tokens, "x");
    Check(fcitx::ResolveSherpaBpeVocab(tokens.string()) == tokens.string(),
          "vocab: use the model's tokens.txt");
    WriteBytes(tmp.path / "bpe.vocab", "x");
    Check(fcitx::ResolveSherpaBpeVocab(tokens.string()) == tokens.string(),
          "vocab: ignore an unverified bpe.vocab");
}

} // namespace

int main() {
    TestHotwordsFile();
    TestHotwordsDefaultLocationFollowsXdg();
    TestHotwordEncodingDetection();
    TestHotwordNormalization();
    TestBpeVocabResolution();

    if (failures == 0) {
        std::cout << "sherpa_paths_test: all checks passed\n";
        return 0;
    }
    std::cerr << "sherpa_paths_test: " << failures << " check(s) failed\n";
    return 1;
}
