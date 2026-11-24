// sherpa_model_verify 单元测试：受控模型路径解析与逐文件校验。
//
// 用**合成清单**验证判定逻辑（文件几字节、摘要本地计算），用**内置清单**
// 验证「后端 → 清单项」映射与版本目录布局；篡改/符号链接用例无需真实模型。
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "asr/utils/sherpa_model_resolver.h"
#include "asr/utils/sherpa_model_verify.h"
#include "model_catalog.h"
#include "model_paths.h"
#include "sha256.h"

namespace {

namespace fs = std::filesystem;
using fcitx::SherpaModelStatus;

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
               ("voice-input-verify-test-" + std::to_string(::getpid()));
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

// ── 合成清单 ────────────────────────────────────────────────────────────────

struct SyntheticFile {
    const char* role;
    const char* name;
    std::string content;
};

std::vector<SyntheticFile> StandardFiles() {
    return {{"encoder", "encoder.onnx", "encoder-bytes"},
            {"decoder", "decoder.onnx", "decoder-bytes"},
            {"joiner", "joiner.onnx", "joiner-bytes"},
            {"tokens", "tokens.txt", "tokens-bytes"}};
}

/// 用清单的真实结构构造一个小模型；摘要由本地计算，不依赖内置常量。
struct SyntheticModel {
    // 声明顺序即构造顺序：specs 保存的 c_str() 指向 digestStorage。
    std::deque<std::string> digestStorage;
    std::vector<std::string> contents;
    std::vector<fcitx::model_manager::ModelFileSpec> specs;
    fcitx::model_manager::ModelSpec spec;

    explicit SyntheticModel(std::vector<SyntheticFile> files) {
        for (const auto& file : files) contents.push_back(file.content);
        for (size_t i = 0; i < files.size(); ++i) {
            fcitx::model_manager::Sha256Digest digest{};
            fcitx::model_manager::HashBuffer(contents[i].data(),
                                             contents[i].size(), &digest);
            digestStorage.push_back(fcitx::model_manager::ToHex(digest));
            specs.push_back({files[i].role, files[i].name,
                             digestStorage.back().c_str(),
                             static_cast<uint64_t>(contents[i].size())});
        }
        spec = {"synthetic-model", "synthetic-version", "synthetic",
                /*streaming=*/false, "", "", 0, specs.data(), specs.size()};
    }

    /// 在 modelsRoot 下按 <id>/<version> 生成受控布局，返回版本目录路径。
    std::string Materialize(const fs::path& modelsRoot) const {
        std::error_code ec;
        const fs::path dir = modelsRoot / spec.id / spec.version;
        fs::create_directories(dir, ec);
        for (size_t i = 0; i < specs.size(); ++i) {
            WriteBytes(dir / specs[i].relativePath, contents[i]);
        }
        return dir.string();
    }
};

/// 一套合成模型 + 它在受控根下的落盘布局，省去每个用例重复拼路径。
struct SyntheticLayout {
    SyntheticModel model;
    std::string root;
    std::string versionDir;

    explicit SyntheticLayout(const fs::path& base,
                             std::vector<SyntheticFile> files = StandardFiles())
        : model(std::move(files)), root((base / "models").string()) {
        versionDir = model.Materialize(root);
    }

    fcitx::SherpaModelVerification Verify() const {
        return fcitx::VerifyModelVersionDir(model.spec, root, versionDir);
    }

    fs::path IdDir() const { return fs::path(root) / model.spec.id; }
};

// ── 校验：内容 ──────────────────────────────────────────────────────────────

void TestVerifyNotInstalled() {
    TempDir tmp;
    SyntheticModel model(StandardFiles());
    const std::string root = (tmp.path / "models").string();
    // 版本目录不存在，但路径按清单拼出（满足"精确路径"前提）。
    auto missing = fcitx::VerifyModelVersionDir(
        model.spec, root,
        root + "/" + model.spec.id + "/" + model.spec.version);
    Check(missing.status == SherpaModelStatus::NotInstalled,
          "verify: absent version dir should report NotInstalled");

    SyntheticLayout layout(tmp.path);
    fs::remove(fs::path(layout.versionDir) / "joiner.onnx");
    auto partial = layout.Verify();
    Check(partial.status == SherpaModelStatus::NotInstalled,
          "verify: missing file should report NotInstalled");
    Check(partial.failedRelativePath == "joiner.onnx",
          "verify: failure should name the missing relative path");
}

void TestVerifyTampered() {
    TempDir tmp;
    // 尺寸相同、字节不同：只能靠摘要发现。
    SyntheticLayout layout(tmp.path);
    WriteBytes(fs::path(layout.versionDir) / "encoder.onnx", "ENCODER-bytes");
    auto tampered = layout.Verify();
    Check(tampered.status == SherpaModelStatus::Tampered,
          "verify: same-size content change should report Tampered");
    Check(tampered.failedRelativePath == "encoder.onnx",
          "verify: tampered file should be named");
    Check(tampered.encoder.empty(), "verify: no paths on failure");

    SyntheticLayout layout2(tmp.path);
    WriteBytes(fs::path(layout2.versionDir) / "tokens.txt", "tokens");
    Check(layout2.Verify().status == SherpaModelStatus::Tampered,
          "verify: truncated file should report Tampered");
}

void TestVerifyAcceptsExactMatch() {
    TempDir tmp;
    SyntheticLayout layout(tmp.path);
    const std::string dir = layout.versionDir;

    auto result = layout.Verify();
    Check(result.status == SherpaModelStatus::Ok,
          "verify: exact match should report Ok");
    Check(result.encoder == dir + "/encoder.onnx" &&
              result.decoder == dir + "/decoder.onnx" &&
              result.joiner == dir + "/joiner.onnx" &&
              result.tokens == dir + "/tokens.txt",
          "verify: all four role paths should come from relativePath");
}

void TestVerifyRejectsMalformedSpec() {
    TempDir tmp;
    SyntheticLayout layout(tmp.path);
    const std::string& root = layout.root;
    const std::string& dir = layout.versionDir;

    // 版本目录必须严格等于 root/<id>/<version>：任意目录不能被认领为受控目录。
    Check(fcitx::VerifyModelVersionDir(layout.model.spec, root,
                                       (tmp.path / "anywhere").string())
                  .status == SherpaModelStatus::InvalidSpec,
          "verify: mismatched version dir should be InvalidSpec");

    auto files = layout.model.specs;
    files[0].relativePath = "../encoder.onnx";
    auto traversal = layout.model.spec;
    traversal.files = files.data();
    Check(fcitx::VerifyModelVersionDir(traversal, root, dir).status ==
              SherpaModelStatus::InvalidSpec,
          "verify: path traversal in relativePath should be InvalidSpec");

    auto missingRole = layout.model.spec;
    missingRole.fileCount = layout.model.specs.size() - 1;
    Check(fcitx::VerifyModelVersionDir(missingRole, root, dir).status ==
              SherpaModelStatus::InvalidSpec,
          "verify: missing tokens role should be InvalidSpec");

    auto files2 = layout.model.specs;
    files2[1].sha256Hex = "not-a-digest";
    auto badDigest = layout.model.spec;
    badDigest.files = files2.data();
    Check(fcitx::VerifyModelVersionDir(badDigest, root, dir).status ==
              SherpaModelStatus::InvalidSpec,
          "verify: malformed sha256 should be InvalidSpec");
}

// ── 校验：符号链接（最终文件 / 受控链中间级） ───────────────────────────────

void TestVerifyRejectsFileSymlink() {
    TempDir tmp;
    SyntheticLayout layout(tmp.path);
    // 内容一致但不是受控发布产物：符号链接必须拒绝。
    const fs::path real = tmp.path / "outside.txt";
    WriteBytes(real, "tokens-bytes");
    const fs::path link = fs::path(layout.versionDir) / "tokens.txt";
    std::error_code ec;
    fs::remove(link, ec);
    fs::create_symlink(real, link, ec);
    Check(!ec, "verify: symlink fixture should be created");
    Check(layout.Verify().status == SherpaModelStatus::Tampered,
          "verify: symlinked model file must be rejected");
}

void TestVerifyRejectsIntermediateSymlinks() {
    // 受控链上任何一级是符号链接都必须拒绝：只在最终文件上做 O_NOFOLLOW
    // 挡不住 models/<id> 被指向别处（整个"受控目录"被重定向）。
    TempDir tmp;

    // (a) models/<id> → 别处（内容完全正确也不接受）。
    {
        SyntheticLayout layout(tmp.path / "a");
        const fs::path moved = tmp.path / "elsewhere" / layout.model.spec.id;
        fs::create_directories(moved.parent_path());
        std::error_code ec;
        fs::rename(layout.IdDir(), moved, ec);
        Check(!ec, "chain: model dir should move");
        fs::create_symlink(moved, layout.IdDir(), ec);
        Check(!ec, "chain: <id> symlink should be created");
        Check(layout.Verify().status == SherpaModelStatus::Tampered,
              "chain: symlinked <id> dir must be rejected");
    }
    // (b) models/<id>/<version> → 别处。
    {
        SyntheticLayout layout(tmp.path / "b");
        const fs::path moved = tmp.path / "moved-version";
        std::error_code ec;
        fs::rename(layout.versionDir, moved, ec);
        Check(!ec, "chain: version dir should move");
        fs::create_symlink(moved, layout.versionDir, ec);
        Check(!ec, "chain: <version> symlink should be created");
        Check(layout.Verify().status == SherpaModelStatus::Tampered,
              "chain: symlinked <version> dir must be rejected");
    }
    // (c) models 根自身 → 别处。
    {
        SyntheticLayout layout(tmp.path / "c");
        const fs::path moved = tmp.path / "moved-models";
        std::error_code ec;
        fs::rename(layout.root, moved, ec);
        Check(!ec, "chain: models root should move");
        fs::create_symlink(moved, layout.root, ec);
        Check(!ec, "chain: models symlink should be created");
        Check(layout.Verify().status == SherpaModelStatus::Tampered,
              "chain: symlinked models root must be rejected");
    }
}

// ── 内置清单与 XDG 路径 ─────────────────────────────────────────────────────

void TestBuiltinCatalogKindBinding() {
    using fcitx::SherpaBackendKind;
    const auto* streaming = fcitx::FindSpecForKind(SherpaBackendKind::Streaming);
    const auto* offline = fcitx::FindSpecForKind(SherpaBackendKind::Offline);
    Check(streaming != nullptr, "catalog: streaming spec should exist");
    Check(offline != nullptr, "catalog: offline spec should exist");
    if (!streaming || !offline) return;

    Check(streaming->streaming && !offline->streaming,
          "catalog: streaming flag must match the backend kind");
    const auto* encoder =
        fcitx::model_manager::FindModelFileByRole(*streaming, "encoder");
    Check(encoder != nullptr &&
              std::string(encoder->relativePath).find(".int8.onnx") !=
                  std::string::npos,
          "catalog: streaming encoder should be the int8 variant");
    Check(fcitx::model_manager::FindModelFileByRole(*offline, "tokens") !=
              nullptr,
          "catalog: offline spec should declare tokens role");
    // 串用会让 sherpa 致命退出，两个后端必须指向不同版本。
    Check(std::string(streaming->id) != offline->id &&
              std::string(streaming->version) != offline->version,
          "catalog: kinds must map to distinct ids and versions");
}

void TestResolveNotInstalled() {
    TempDir tmp;
    EnvGuard guard({"XDG_DATA_HOME", "HOME"});
    ::setenv("XDG_DATA_HOME", tmp.path.c_str(), 1);

    const auto resolved =
        fcitx::ResolveControlledSherpaModel(fcitx::SherpaBackendKind::Streaming);
    Check(resolved.status == SherpaModelStatus::NotInstalled,
          "resolve: empty data dir should report NotInstalled");
    Check(resolved.encoder.empty(), "resolve: no paths when not installed");
    Check(!resolved.modelId.empty(), "resolve: id reported for diagnostics");
    Check(fcitx::ResolveControlledSherpaModel(fcitx::SherpaBackendKind::Offline)
                  .status == SherpaModelStatus::NotInstalled,
          "resolve: builtin offline spec should be well-formed");
}

void TestResolveAbsoluteXdg() {
    TempDir tmp;
    EnvGuard guard({"XDG_DATA_HOME", "HOME"});
    ::setenv("XDG_DATA_HOME", tmp.path.c_str(), 1);

    const auto* spec = fcitx::FindSpecForKind(fcitx::SherpaBackendKind::Offline);
    if (!spec) {
        Check(false, "resolve: offline spec should exist");
        return;
    }
    const fs::path dir =
        tmp.path / "fcitx5/voice-input/models" / spec->id / spec->version;
    fs::create_directories(dir);
    for (size_t i = 0; i < spec->fileCount; ++i) {
        WriteBytes(dir / spec->files[i].relativePath, "wrong-bytes");
    }

    const auto resolved =
        fcitx::ResolveControlledSherpaModel(fcitx::SherpaBackendKind::Offline);
    Check(resolved.status == SherpaModelStatus::Tampered,
          "resolve: incorrectly sized files should report Tampered");
    Check(resolved.versionDir == dir.string(),
          "resolve: version dir should be "
          "<data home>/fcitx5/voice-input/models/<id>/<version>");
}

void TestResolveIgnoresRelativeXdg() {
    TempDir tmp;
    EnvGuard guard({"XDG_DATA_HOME", "HOME"});
    ::setenv("XDG_DATA_HOME", "relative/data/home", 1);
    ::setenv("HOME", tmp.path.c_str(), 1);

    const auto resolved =
        fcitx::ResolveControlledSherpaModel(fcitx::SherpaBackendKind::Streaming);
    Check(resolved.status == SherpaModelStatus::NotInstalled,
          "relative xdg: relative XDG_DATA_HOME must be ignored");
    Check(resolved.versionDir.rfind((tmp.path / ".local/share").string(), 0) ==
              0,
          "relative xdg: path must come from $HOME/.local/share");
}

void TestResolveKindMismatch() {
    TempDir tmp;
    EnvGuard guard({"XDG_DATA_HOME", "HOME"});
    ::setenv("XDG_DATA_HOME", tmp.path.c_str(), 1);

    const auto* offline =
        fcitx::FindSpecForKind(fcitx::SherpaBackendKind::Offline);
    const auto* streaming =
        fcitx::FindSpecForKind(fcitx::SherpaBackendKind::Streaming);
    if (!offline || !streaming) {
        Check(false, "mismatch: both specs should exist");
        return;
    }
    const fs::path dir = tmp.path / "fcitx5/voice-input/models" /
                         streaming->id / streaming->version;
    fs::create_directories(dir);
    for (size_t i = 0; i < offline->fileCount; ++i) {
        WriteBytes(dir / offline->files[i].relativePath, "wrong-model");
    }
    Check(fcitx::ResolveControlledSherpaModel(
              fcitx::SherpaBackendKind::Streaming)
                  .status == SherpaModelStatus::NotInstalled,
          "mismatch: offline files under streaming id must be rejected");
}

void TestResolveRejectsIntermediateSymlink() {
    TempDir tmp;
    EnvGuard guard({"XDG_DATA_HOME", "HOME"});
    ::setenv("XDG_DATA_HOME", tmp.path.c_str(), 1);

    // 端到端：models/<id> 指向外部目录，即使文件齐全也必须是 Tampered。
    const auto* spec =
        fcitx::FindSpecForKind(fcitx::SherpaBackendKind::Streaming);
    if (!spec) {
        Check(false, "chain: streaming spec should exist");
        return;
    }
    const fs::path root = tmp.path / "fcitx5/voice-input/models";
    const fs::path outside = tmp.path / "outside" / spec->id / spec->version;
    fs::create_directories(outside);
    for (size_t i = 0; i < spec->fileCount; ++i) {
        WriteBytes(outside / spec->files[i].relativePath, "any");
    }
    fs::create_directories(root);
    std::error_code ec;
    fs::create_symlink(outside, root / spec->id, ec);
    Check(!ec, "chain: <id> symlink should be created");

    Check(fcitx::ResolveControlledSherpaModel(
              fcitx::SherpaBackendKind::Streaming)
                  .status == SherpaModelStatus::Tampered,
          "chain: symlinked <id> under models must be rejected");
}

} // namespace

int main() {
    TestVerifyNotInstalled();
    TestVerifyTampered();
    TestVerifyAcceptsExactMatch();
    TestVerifyRejectsMalformedSpec();
    TestVerifyRejectsFileSymlink();
    TestVerifyRejectsIntermediateSymlinks();

    TestBuiltinCatalogKindBinding();
    TestResolveNotInstalled();
    TestResolveAbsoluteXdg();
    TestResolveIgnoresRelativeXdg();
    TestResolveKindMismatch();
    TestResolveRejectsIntermediateSymlink();

    if (failures == 0) {
        std::cout << "sherpa_model_verify_test: all checks passed\n";
        return 0;
    }
    std::cerr << "sherpa_model_verify_test: " << failures
              << " check(s) failed\n";
    return 1;
}
