// model_manager 单元测试（一）：清单、路径、SHA-256、协议事件。
//
// 不依赖网络、真实模型或 sherpa 运行库。
// 解包与锁的用例在 model_manager_extract_test.cpp。

#include <chrono>
#include <cstring>
#include <string>

#include <sys/stat.h>

#include "model_catalog.h"
#include "model_installer.h"
#include "model_paths.h"
#include "sha256.h"

#include "support/model_manager_test_util.h"

namespace {

using namespace model_manager_test;
using fcitx::model_manager::DataHomeDir;
using fcitx::model_manager::DataRootDir;
using fcitx::model_manager::FindModelFile;
using fcitx::model_manager::FindModelFileByRole;
using fcitx::model_manager::FindModelSpec;
using fcitx::model_manager::HashBuffer;
using fcitx::model_manager::HashFile;
using fcitx::model_manager::HashFileLimited;
using fcitx::model_manager::InstallMarkerPath;
using fcitx::model_manager::IsKnownRole;
using fcitx::model_manager::IsSafePathSegment;
using fcitx::model_manager::IsSafeRelativePath;
using fcitx::model_manager::ListModelSpecs;
using fcitx::model_manager::ModelDir;
using fcitx::model_manager::ParseHexDigest;
using fcitx::model_manager::Sha256;
using fcitx::model_manager::Sha256Digest;
using fcitx::model_manager::ToHex;
using fcitx::model_manager::VersionDir;

// ── 清单 ─────────────────────────────────────────────────────────────────────

void TestCatalogHasTwoSpecs() {
    size_t count = 0;
    const ModelSpec* specs = ListModelSpecs(&count);
    Check(specs != nullptr && count == 2, "catalog: exactly two specs");

    const ModelSpec* online = FindModelSpec("online-zipformer");
    const ModelSpec* offline = FindModelSpec("offline-x-asr");
    Check(online != nullptr, "catalog: online-zipformer exists");
    Check(offline != nullptr, "catalog: offline-x-asr exists");
    Check(FindModelSpec("nope") == nullptr, "catalog: unknown id returns null");
    Check(FindModelSpec("") == nullptr, "catalog: empty id returns null");
    if (!online || !offline) return;

    Check(online->streaming, "catalog: online spec is streaming");
    Check(!offline->streaming, "catalog: offline spec is not streaming");
    Check(std::string(online->id) == "online-zipformer",
          "catalog: online id stable");
    Check(std::string(offline->id) == "offline-x-asr",
          "catalog: offline id stable");
}

void TestCatalogFilesAreComplete() {
    const ModelSpec* specs[] = {FindModelSpec("online-zipformer"),
                                FindModelSpec("offline-x-asr")};
    for (const ModelSpec* spec : specs) {
        if (!spec) continue;
        Check(spec->fileCount == 4,
              std::string("catalog: 4 files for ") + spec->id);
        for (const char* role : {"encoder", "decoder", "joiner", "tokens"}) {
            const ModelFileSpec* f = FindModelFileByRole(*spec, role);
            Check(f != nullptr, std::string("catalog: role ") + role +
                                    " present for " + spec->id);
            if (!f) continue;
            Check(std::strlen(f->sha256Hex) == 64,
                  std::string("catalog: 64-hex digest for ") + f->relativePath);
            Check(f->size > 0,
                  std::string("catalog: nonzero size for ") + f->relativePath);
            Check(std::strchr(f->relativePath, '/') == nullptr,
                  std::string("catalog: single-component path for ") +
                      f->relativePath);
            Sha256Digest digest{};
            Check(ParseHexDigest(f->sha256Hex, &digest),
                  std::string("catalog: digest parses for ") + f->relativePath);
            // 摘要不得是占位值。
            Check(!(digest[0] == 0 && digest[16] == 0 && digest[31] == 0),
                  std::string("catalog: digest not trivial for ") +
                      f->relativePath);
        }
        Sha256Digest archive{};
        Check(ParseHexDigest(spec->archiveSha256Hex, &archive),
              std::string("catalog: archive digest parses for ") + spec->id);
        Check(spec->archiveSize > 0,
              std::string("catalog: archive size set for ") + spec->id);
        Check(std::strncmp(spec->archiveUrl, "https://", 8) == 0,
              std::string("catalog: archive URL is https for ") + spec->id);
        Check(std::strstr(spec->archiveUrl, spec->version) != nullptr,
              std::string("catalog: URL references pinned version for ") +
                  spec->id);
    }
}

void TestCatalogFileLookup() {
    const ModelSpec* offline = FindModelSpec("offline-x-asr");
    if (!offline) return;

    Check(FindModelFile(*offline, "tokens.txt") != nullptr,
          "catalog: offline tokens.txt registered");
    Check(FindModelFile(*offline, "missing.onnx") == nullptr,
          "catalog: unknown relative path returns null");
    Check(FindModelFileByRole(*offline, "bpe.model") == nullptr,
          "catalog: unregistered role returns null");

    const ModelSpec* online = FindModelSpec("online-zipformer");
    if (!online) return;
    const ModelFileSpec* onlineEncoder = FindModelFileByRole(*online, "encoder");
    const ModelFileSpec* offlineEncoder =
        FindModelFileByRole(*offline, "encoder");
    Check(onlineEncoder && offlineEncoder &&
              std::string(onlineEncoder->relativePath) !=
                  offlineEncoder->relativePath,
          "catalog: online/offline encoder filenames differ");
    Check(FindModelFile(*online, offlineEncoder->relativePath) == nullptr,
          "catalog: offline filename not valid for online spec");

    // offline 的 encoder/decoder 必须以归档实测值为准（HF 那份是另一次导出）。
    Check(offlineEncoder &&
              std::string(offlineEncoder->sha256Hex) ==
                  "fea6a8d0066b327401b603a7deff24e685379f523771bafd5127aa572018951c",
          "catalog: offline encoder digest is the archive-verified value");
    Check(offlineEncoder && offlineEncoder->size == 161015628,
          "catalog: offline encoder size is the archive-verified value");
}

void TestIsKnownRole() {
    Check(IsKnownRole("encoder"), "role: encoder known");
    Check(IsKnownRole("tokens"), "role: tokens known");
    Check(!IsKnownRole("bpe.model"), "role: bpe.model unknown");
    Check(!IsKnownRole(""), "role: empty unknown");
}

// ── 路径 ─────────────────────────────────────────────────────────────────────

void TestPathSegments() {
    Check(IsSafePathSegment("online-zipformer"), "path: normal segment safe");
    Check(IsSafePathSegment("sherpa-onnx-x-asr-2026-06-03"),
          "path: version segment safe");
    Check(!IsSafePathSegment(""), "path: empty segment unsafe");
    Check(!IsSafePathSegment("."), "path: dot segment unsafe");
    Check(!IsSafePathSegment(".."), "path: dotdot segment unsafe");
    Check(!IsSafePathSegment("../etc"), "path: traversal segment unsafe");
    Check(!IsSafePathSegment("a/b"), "path: slash segment unsafe");
    Check(!IsSafePathSegment(".hidden"), "path: leading dot unsafe");

    Check(IsSafeRelativePath("tokens.txt"), "path: plain file safe");
    Check(!IsSafeRelativePath(""), "path: empty relative unsafe");
    Check(!IsSafeRelativePath("/etc/passwd"), "path: absolute relative unsafe");
    Check(!IsSafeRelativePath("../x"), "path: traversal relative unsafe");
    Check(!IsSafeRelativePath("a/../../b"), "path: inner traversal unsafe");
    Check(!IsSafeRelativePath("a//b"), "path: empty component unsafe");
    Check(!IsSafeRelativePath("a/./b"), "path: dot component unsafe");
}

void TestXdgResolution() {
    TempDir tmp;
    EnvGuard guard({"XDG_DATA_HOME", "HOME"});

    ::setenv("XDG_DATA_HOME", tmp.path.c_str(), 1);
    Check(DataHomeDir() == tmp.path.string(), "xdg: absolute XDG honored");
    Check(DataRootDir() == (tmp.path / "fcitx5/voice-input/models").string(),
          "xdg: data root composed under XDG");

    ::setenv("XDG_DATA_HOME", "relative/path", 1);
    ::setenv("HOME", (tmp.path / "home").c_str(), 1);
    Check(DataHomeDir() == (tmp.path / "home/.local/share").string(),
          "xdg: relative XDG ignored, HOME fallback used");

    ::unsetenv("XDG_DATA_HOME");
    ::unsetenv("HOME");
    Check(DataHomeDir().empty(), "xdg: no XDG and no HOME yields empty");
    Check(DataRootDir().empty(), "xdg: empty data root when no base");
    Check(ModelDir("online-zipformer").empty(),
          "xdg: model dir empty without base");
    Check(VersionDir("online-zipformer", "v").empty(),
          "xdg: version dir empty without base");
}

void TestPathComposition() {
    TempDir tmp;
    EnvGuard guard({"XDG_DATA_HOME", "HOME"});
    ::setenv("XDG_DATA_HOME", tmp.path.c_str(), 1);

    const std::string expected =
        (tmp.path / "fcitx5/voice-input/models/offline-x-asr/v1").string();
    Check(VersionDir("offline-x-asr", "v1") == expected,
          "path: version dir layout matches contract");

    const std::string versionDir = VersionDir("offline-x-asr", "v1");
    Check(InstallMarkerPath(versionDir) == versionDir + "/install.json",
          "path: marker path is install.json inside version dir");

    Check(VersionDir("../evil", "v1").empty(), "path: traversal id rejected");
    Check(VersionDir("id", "../evil").empty(), "path: traversal version rejected");
    Check(ModelDir("..").empty(), "path: dotdot id rejected");
}

// ── SHA-256 ──────────────────────────────────────────────────────────────────

void TestSha256KnownVectors() {
    Sha256Digest digest{};
    Check(HashBuffer("abc", 3, &digest), "sha256: hash 'abc'");
    Check(ToHex(digest) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256: 'abc' matches NIST vector");

    Sha256Digest empty{};
    Check(HashBuffer("", 0, &empty), "sha256: hash empty");
    Check(ToHex(empty) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256: empty string matches NIST vector");

    Sha256 incremental;
    Check(incremental.valid(), "sha256: incremental digest initialises");
    incremental.Update("a", 1);
    incremental.Update("bc", 2);
    Sha256Digest incrementalDigest{};
    Check(incremental.Final(&incrementalDigest),
          "sha256: incremental finalises");
    Check(ToHex(incrementalDigest) == ToHex(digest),
          "sha256: incremental equals one-shot");
}

void TestSha256HexParsing() {
    Sha256Digest digest{};
    Check(ParseHexDigest(std::string(64, 'a'), &digest),
          "hex: 64 lowercase accepted");
    Check(ParseHexDigest(std::string(64, 'A'), &digest),
          "hex: 64 uppercase accepted");
    Check(!ParseHexDigest(std::string(63, 'a'), &digest), "hex: short rejected");
    Check(!ParseHexDigest(std::string(65, 'a'), &digest), "hex: long rejected");
    Check(!ParseHexDigest(std::string(64, 'z'), &digest),
          "hex: non-hex rejected");
    Check(!ParseHexDigest("", &digest), "hex: empty rejected");
}

void TestSha256RejectsInvalidArguments() {
    Sha256Digest digest{};
    // 空指针 + 非零长度是无效调用，必须失败而不是按 len 读取空指针。
    Check(!HashBuffer(nullptr, 4, &digest),
          "sha256: null buffer with length rejected");
    Check(HashBuffer(nullptr, 0, &digest),
          "sha256: null buffer with zero length accepted");
    Check(!HashBuffer("abc", 3, nullptr), "sha256: null output rejected");
}

void TestSha256File() {
    TempDir tmp;
    const fs::path file = tmp.path / "data.bin";
    WriteText(file, "abc");

    Sha256Digest digest{};
    uint64_t size = 0;
    Check(HashFile(file.string(), &digest, &size), "sha256: hash file");
    Check(size == 3, "sha256: file size reported");
    Check(ToHex(digest) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256: file content digest correct");

    Check(!HashFile((tmp.path / "missing.bin").string(), &digest, &size),
          "sha256: missing file fails");

    // 符号链接不被跟随（O_NOFOLLOW）。
    const fs::path link = tmp.path / "link.bin";
    std::error_code ec;
    fs::create_symlink(file, link, ec);
    if (!ec) {
        Check(!HashFile(link.string(), &digest, &size),
              "sha256: symlink rejected, not followed");
    }

    // 目录不是模型文件。
    Check(!HashFile(tmp.path.string(), &digest, &size),
          "sha256: directory rejected");

    Check(!HashFileLimited(file.string(), 2, &digest, &size),
          "sha256: size limit enforced");
    Check(HashFileLimited(file.string(), 3, &digest, &size),
          "sha256: size limit inclusive at boundary");

    // FIFO 必须快速失败，绝不能阻塞（open 上的 O_NONBLOCK + fstat 类型校验）。
    const fs::path fifo = tmp.path / "pipe";
    if (::mkfifo(fifo.c_str(), 0600) == 0) {
        const auto start = std::chrono::steady_clock::now();
        const bool fifoResult = HashFile(fifo.string(), &digest, &size);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
        Check(!fifoResult, "sha256: FIFO rejected, not treated as model file");
        Check(elapsed.count() < 1000, "sha256: FIFO check did not block");
    }
}

// ── 安装选项默认值 ───────────────────────────────────────────────────────────

void TestInstallOptionsDefaults() {
    // 试加载默认开启且有限时：发布前验收不可静默跳过。
    const fcitx::model_manager::InstallOptions options;
    Check(options.runTrialLoad, "install: trial load enabled by default");
    Check(options.trialTimeoutMs > 0, "install: trial timeout set by default");
}

} // namespace

int main() {
    TestCatalogHasTwoSpecs();
    TestCatalogFilesAreComplete();
    TestCatalogFileLookup();
    TestIsKnownRole();

    TestPathSegments();
    TestXdgResolution();
    TestPathComposition();

    TestSha256KnownVectors();
    TestSha256HexParsing();
    TestSha256RejectsInvalidArguments();
    TestSha256File();

    TestInstallOptionsDefaults();

    return Report("model_manager_test");
}
