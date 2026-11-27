// sherpa_paths 单元测试：热词文件解析、编码判定与不可读文件降级。
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

#include <sys/stat.h>
#include <unistd.h>

#include "asr/utils/sherpa_paths.h"
#include "model_paths.h"

namespace {

namespace fs = std::filesystem;

int failures = 0;
bool skipped = false;

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

void TestUnreadableHotwordsDegrade() {
    // issue #82：存在但不可读的热词文件会被 sherpa 的 InitHotwords()
    // 以 _Exit(-1) 终止宿主进程。addon 必须在交给 sherpa 前就降级。
    //
    // root 绕过 DAC（`chmod 000` 仍能打开），这时用例无法构造不可读
    // 状态，只能报告却没有真正跑到断言：返回 77 让 CTest 记为 SKIP，
    // 不能拿“通过”宣称覆盖。
    if (::geteuid() == 0) {
        // 不提前退出：其余用例仍可运行，最终由 main() 统一回报 77。
        std::cerr << "SKIP: unreadable-hotwords case needs a non-root uid "
                     "(root bypasses DAC, chmod 000 does not revoke read)\n";
        skipped = true;
        return;
    }

    TempDir tmp;
    EnvGuard guard({"XDG_DATA_HOME", "XDG_CACHE_HOME", "HOME"});

    const fs::path blocked = tmp.path / "hotwords-blocked.txt";
    WriteBytes(blocked, "编 程\n");
    ::chmod(blocked.c_str(), 0);

    Check(fcitx::ResolveSherpaHotwordsFile(blocked.string(), "test").empty(),
          "unreadable: configured file should be ignored");

    // 默认探测路径同样要拒绝不可读文件：否则用户只需把热词放在默认位置、
    // 权限被改坏就会杀掉 fcitx5。
    const fs::path dataShare = tmp.path / "data";
    const fs::path defaultHotwords =
        dataShare / "fcitx5/voice-input/hotwords.txt";
    WriteBytes(defaultHotwords, "编 程\n");
    ::chmod(defaultHotwords.c_str(), 0);
    ::setenv("XDG_DATA_HOME", dataShare.c_str(), 1);
    ::setenv("HOME", (tmp.path / "home").c_str(), 1);
    Check(fcitx::ResolveSherpaHotwordsFile("", "test").empty(),
          "unreadable: default location should be ignored");

    // Prepare 的三种编码都必须在开文件失败时返回空串（调用方据此关热词）。
    for (auto encoding : {fcitx::HotwordEncoding::CjkChar,
                          fcitx::HotwordEncoding::Bpe,
                          fcitx::HotwordEncoding::BpePrefixed}) {
        Check(fcitx::PrepareSherpaHotwordsFile(blocked.string(), encoding, "test")
                  .empty(),
              "unreadable: Prepare must drop hotwords for every encoding");
    }

    ::chmod(blocked.c_str(), 0644);
    ::chmod(defaultHotwords.c_str(), 0644);
}

void TestEmptyHotwordsDegrade() {
    // 空 / 仅空白的热词文件同样会让 sherpa 建一个空上下文图；两种编码都应
    // 提前返回空串，而不是把文件路径传下去。
    TempDir tmp;
    const fs::path empty = tmp.path / "hotwords-empty.txt";
    WriteBytes(empty, "");
    Check(fcitx::ResolveSherpaHotwordsFile(empty.string(), "test") ==
              empty.string(),
          "empty: readability check keeps a readable empty file");

    const fs::path blank = tmp.path / "hotwords-blank.txt";
    WriteBytes(blank, "\n \t\r\n");
    Check(fcitx::PrepareSherpaHotwordsFile(
              empty.string(), fcitx::HotwordEncoding::CjkChar, "test")
              .empty(),
          "empty: cjkchar should drop an empty hotwords file");
    Check(fcitx::PrepareSherpaHotwordsFile(empty.string(),
                                           fcitx::HotwordEncoding::Bpe, "test")
              .empty(),
          "empty: bpe should drop an empty hotwords file");
    Check(fcitx::PrepareSherpaHotwordsFile(
              blank.string(), fcitx::HotwordEncoding::CjkChar, "test")
              .empty(),
          "empty: whitespace-only file counts as empty");
    Check(fcitx::PrepareSherpaHotwordsFile(
              (tmp.path / "missing.txt").string(),
              fcitx::HotwordEncoding::CjkChar, "test")
              .empty(),
          "empty: missing file must be dropped before sherpa sees it");
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
    TestUnreadableHotwordsDegrade();
    TestEmptyHotwordsDegrade();
    TestBpeVocabResolution();

    if (failures == 0) {
        if (skipped) {
            std::cout << "sherpa_paths_test: all executed checks passed; "
                         "unreadable-file case skipped (root)\n";
            return 77;  // CTest SKIP_RETURN_CODE
        }
        std::cout << "sherpa_paths_test: all checks passed\n";
        return 0;
    }
    std::cerr << "sherpa_paths_test: " << failures << " check(s) failed\n";
    return 1;
}
