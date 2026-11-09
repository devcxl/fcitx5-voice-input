// sherpa_paths 单元测试：模型目录探测与热词文件解析。
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "asr/utils/sherpa_paths.h"

namespace {

namespace fs = std::filesystem;

int failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

void Touch(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path).put('x');
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

void MakeModelDir(const fs::path& dir, bool int8) {
    Touch(dir / "encoder-epoch-99-avg-1.onnx");
    if (int8) {
        Touch(dir / "encoder-epoch-99-avg-1.int8.onnx");
    }
    Touch(dir / "decoder-epoch-99-avg-1.onnx");
    Touch(dir / "joiner-epoch-99-avg-1.onnx");
    Touch(dir / "tokens.txt");
}

void TestInt8Preferred() {
    TempDir tmp;
    const fs::path modelDir = tmp.path / "model";
    MakeModelDir(modelDir, /*int8=*/true);

    const auto paths = fcitx::ResolveSherpaModelPaths(modelDir.string(), {},
                                                      "test");
    Check(paths.valid, "int8: model dir should resolve");
    Check(paths.encoder.find(".int8.onnx") != std::string::npos,
          "int8: encoder should prefer int8 variant");
    Check(paths.tokens == (modelDir / "tokens.txt").string(),
          "int8: tokens.txt should be resolved");
}

void TestSubdirectoryScan() {
    TempDir tmp;
    MakeModelDir(tmp.path / "models" / "zipformer", /*int8=*/false);

    const auto paths = fcitx::ResolveSherpaModelPaths(
        (tmp.path / "models").string(), {}, "test");
    Check(paths.valid, "subdir: one level below modelDir should resolve");
    Check(paths.encoder.find("zipformer") != std::string::npos,
          "subdir: encoder should come from the scanned subdirectory");
    Check(paths.encoder.find(".int8.onnx") == std::string::npos,
          "subdir: fp32 fallback should be used when no int8 file exists");
}

void TestExplicitOverrides() {
    TempDir tmp;
    const fs::path dir = tmp.path / "explicit";
    MakeModelDir(dir, /*int8=*/false);
    // 默认目录里放一份更"新"的 int8 模型，显式配置必须优先。
    MakeModelDir(tmp.path / "default", /*int8=*/true);

    fcitx::SherpaModelPathOverrides overrides;
    overrides.encoder = (dir / "encoder-epoch-99-avg-1.onnx").string();
    overrides.decoder = (dir / "decoder-epoch-99-avg-1.onnx").string();
    overrides.joiner = (dir / "joiner-epoch-99-avg-1.onnx").string();
    overrides.tokens = (dir / "tokens.txt").string();

    const auto paths = fcitx::ResolveSherpaModelPaths(
        (tmp.path / "default").string(), overrides, "test");
    Check(paths.valid, "overrides: explicit files should resolve");
    Check(paths.encoder == overrides.encoder,
          "overrides: explicit encoder should win over directory scan");
    Check(paths.encoder.find("explicit") != std::string::npos,
          "overrides: encoder should be the configured file");
}

void TestHotwordsFile() {
    TempDir tmp;
    const fs::path hotwords = tmp.path / "hotwords.txt";
    Touch(hotwords);

    Check(fcitx::ResolveSherpaHotwordsFile(hotwords.string(), "test") ==
              hotwords.string(),
          "hotwords: configured file should be returned");

    Check(fcitx::ResolveSherpaHotwordsFile(
              (tmp.path / "missing.txt").string(), "test")
              .empty(),
          "hotwords: missing configured file should be ignored");
}

void WriteText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << text;
}

void TestHotwordEncodingDetection() {
    TempDir tmp;

    const fs::path prefixed = tmp.path / "tokens-prefixed.txt";
    WriteText(prefixed, "<blk> 0\n<sos/eos> 1\n▁编 2\n▁程 3\n▁FRONT 4\n");
    Check(fcitx::DetectHotwordEncoding(prefixed.string()) ==
              fcitx::HotwordEncoding::BpePrefixed,
          "encoding: ▁-prefixed Han tokens should select bpe(per-char)");

    const fs::path bare = tmp.path / "tokens-bare.txt";
    WriteText(bare, "<blk> 0\n编 2\n程 3\n");
    Check(fcitx::DetectHotwordEncoding(bare.string()) ==
              fcitx::HotwordEncoding::CjkChar,
          "encoding: bare Han tokens should select cjkchar");

    const fs::path english = tmp.path / "tokens-en.txt";
    WriteText(english, "<blk> 0\n▁THE 2\nED 3\n");
    Check(fcitx::DetectHotwordEncoding(english.string()) ==
              fcitx::HotwordEncoding::Bpe,
          "encoding: no Han tokens should select bpe");
}

void TestHotwordNormalization() {
    TempDir tmp;
    const fs::path hotwords = tmp.path / "hotwords.txt";
    WriteText(hotwords, "编程\nFRONT\n深度学习 :3.5\n\n  \n");

    const fs::path cache = tmp.path / "cache";
    ::setenv("XDG_CACHE_HOME", cache.c_str(), 1);

    const std::string prepared = fcitx::PrepareSherpaHotwordsFile(
        hotwords.string(), fcitx::HotwordEncoding::BpePrefixed, "test");
    Check(prepared == (cache / "fcitx5/voice-input/hotwords-bpe.txt").string(),
          "normalize: derived file should land in the cache dir");

    std::ifstream in(prepared);
    std::string content((std::istreambuf_iterator<char>(in)),
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
    Touch(tokens);

    Check(fcitx::ResolveSherpaBpeVocab(tokens.string()) == tokens.string(),
          "vocab: fall back to tokens.txt when bpe.vocab is absent");

    const fs::path vocab = tmp.path / "bpe.vocab";
    Touch(vocab);
    Check(fcitx::ResolveSherpaBpeVocab(tokens.string()) == vocab.string(),
          "vocab: prefer bpe.vocab when present");
}

} // namespace

int main() {
    TestInt8Preferred();
    TestSubdirectoryScan();
    TestExplicitOverrides();
    TestHotwordsFile();
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
