// model_manager 单元测试（三）：安全文件系统原语与有界摘要。
//
// 回归重点（对应复审意见）：
//   - 已存在目录的权限绝不被修改；
//   - 暂存目录必须独占创建，不可能复用旧目录；
//   - 有界复制/摘要在读取前就用 fstat 拒绝过大输入；
//   - FIFO 不会阻塞；
//   - 原子交换失败时不得改动任何一方。

#include <chrono>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs_safe.h"
#include "installed_model.h"
#include "model_paths.h"
#include "sha256.h"

#include "support/model_manager_test_util.h"

namespace {

using namespace model_manager_test;
using fcitx::model_manager::CheckInstalledModel;
using fcitx::model_manager::CopyFileBounded;
using fcitx::model_manager::CopyStatus;
using fcitx::model_manager::CopyStatusName;
using fcitx::model_manager::DigestFile;
using fcitx::model_manager::CheckDirChain;
using fcitx::model_manager::ChainStatus;
using fcitx::model_manager::EnsureDir;
using fcitx::model_manager::EnsureDirUnder;
using fcitx::model_manager::FsyncDir;
using fcitx::model_manager::FileDigestStatus;
using fcitx::model_manager::FindModelSpec;
using fcitx::model_manager::InstalledState;
using fcitx::model_manager::MakeUniqueDir;
using fcitx::model_manager::NoReplaceStatus;
using fcitx::model_manager::RenameNoReplaceAt;

mode_t ModeOf(const fs::path& p) {
    struct stat st {};
    if (::lstat(p.c_str(), &st) != 0) return 0;
    return st.st_mode & 07777;
}

// ── 目录：只设置新建目录的权限 ───────────────────────────────────────────────

void TestEnsureDirDoesNotChmodExisting() {
    TempDir tmp;

    // 用户已有目录（0700）：确保不被改成 0755。
    const fs::path userDir = tmp.path / "user-owned";
    fs::create_directories(userDir);
    ::chmod(userDir.c_str(), 0700);

    std::string err;
    Check(EnsureDir(userDir.string(), 0755, &err),
          "fs: EnsureDir succeeds on existing directory");
    Check(ModeOf(userDir) == 0700,
          "fs: EnsureDir must NOT chmod an existing directory");

    // 新建目录才应用 mode。
    const fs::path fresh = tmp.path / "fresh/child";
    Check(EnsureDir(fresh.string(), 0700, &err), "fs: EnsureDir creates nested");
    Check(ModeOf(fresh) == 0700, "fs: EnsureDir applies mode to new directory");
}

void TestEnsureDirRejectsNonDirectory() {
    TempDir tmp;
    std::string err;

    const fs::path file = tmp.path / "plain-file";
    WriteText(file, "x");
    Check(!EnsureDir(file.string(), 0700, &err),
          "fs: EnsureDir rejects existing regular file");
    Check(!err.empty(), "fs: EnsureDir reports reason for non-directory");

    // 指向目录的符号链接也不接受（避免把写入导向别处）。
    const fs::path realDir = tmp.path / "real";
    fs::create_directories(realDir);
    const fs::path link = tmp.path / "dirlink";
    std::error_code ec;
    fs::create_directory_symlink(realDir, link, ec);
    if (!ec) {
        Check(!EnsureDir(link.string(), 0700, &err),
              "fs: EnsureDir rejects symlinked directory");
    }
}

void TestEnsureDirUnderAllowsTrustedSymlinkedBase() {
    TempDir tmp;

    // 模拟用户环境：HOME/XDG 是符号链接（很常见），受控段在其下。
    const fs::path realBase = tmp.path / "real";
    const fs::path trusted = realBase / "fcitx5/voice-input";
    fs::create_directories(trusted);
    const fs::path linkBase = tmp.path / "home-link";
    std::error_code ec;
    fs::create_directory_symlink(realBase, linkBase, ec);
    if (ec) return;

    const std::string trustedViaLink =
        (linkBase / "fcitx5/voice-input").string();
    const std::string models = trustedViaLink + "/models";
    std::string err;
    Check(EnsureDirUnder(models, trustedViaLink, 0700, &err),
          "fs: EnsureDirUnder accepts a trusted (symlinked) base path");
    Check(fs::is_directory(realBase / "fcitx5/voice-input/models"),
          "fs: EnsureDirUnder created the controlled dir");

    // 受控段内出现符号链接必须拒绝。
    const fs::path outside = tmp.path / "outside";
    fs::create_directories(outside);
    const std::string idDir = models + "/online-zipformer";
    std::error_code ec2;
    fs::create_directory_symlink(outside, idDir, ec2);
    if (!ec2) {
        Check(!EnsureDirUnder(idDir + "/v1", trustedViaLink, 0700, &err),
              "fs: EnsureDirUnder rejects symlink below the trusted base");
    }
}

// ── 暂存目录：独占创建 ───────────────────────────────────────────────────────

void TestMakeUniqueDirIsExclusive() {
    TempDir tmp;
    std::string err;

    const std::string a = MakeUniqueDir(tmp.path.string(), "offline-x-asr", &err);
    const std::string b = MakeUniqueDir(tmp.path.string(), "offline-x-asr", &err);
    Check(!a.empty() && !b.empty(), "fs: MakeUniqueDir creates directories");
    Check(a != b, "fs: MakeUniqueDir never reuses an existing directory");
    Check(ModeOf(a) == 0700, "fs: MakeUniqueDir is private (0700)");
    Check(fs::is_directory(a) && fs::is_directory(b),
          "fs: MakeUniqueDir results are directories");

    // label 不能逃出根目录。
    const std::string escape =
        MakeUniqueDir(tmp.path.string(), "../../etc", &err);
    if (!escape.empty()) {
        Check(escape.rfind(tmp.path.string(), 0) == 0,
              "fs: MakeUniqueDir label cannot escape the root");
    }

    Check(MakeUniqueDir("", "x", &err).empty(),
          "fs: MakeUniqueDir fails without a root");
}

// ── 有界复制 ─────────────────────────────────────────────────────────────────

void TestCopyFileBounded() {
    TempDir tmp;
    std::string err;

    const fs::path src = tmp.path / "src.bin";
    WriteText(src, "0123456789");

    const fs::path dst = tmp.path / "dst.bin";
    Check(CopyFileBounded(src.string(), dst.string(), 10, {}, &err) ==
              CopyStatus::Ok,
          "fs: bounded copy succeeds at the limit");
    Check(fs::file_size(dst) == 10, "fs: bounded copy preserved content");

    // 目标已存在 → 拒绝（O_EXCL），绝不覆盖。
    Check(CopyFileBounded(src.string(), dst.string(), 10, {}, &err) ==
              CopyStatus::IoError,
          "fs: bounded copy refuses to overwrite an existing target");

    // 超限：在 fstat 阶段就拒绝。
    const fs::path dst2 = tmp.path / "dst2.bin";
    Check(CopyFileBounded(src.string(), dst2.string(), 5, {}, &err) ==
              CopyStatus::SourceTooLarge,
          "fs: bounded copy rejects oversized source");
    Check(!fs::exists(dst2), "fs: oversized source leaves no target");

    // 取消。
    const fs::path dst3 = tmp.path / "dst3.bin";
    Check(CopyFileBounded(src.string(), dst3.string(), 10, [] { return true; },
                          &err) == CopyStatus::Cancelled,
          "fs: bounded copy honours cancellation");

    // FIFO 不得阻塞。
    const fs::path fifo = tmp.path / "pipe";
    if (::mkfifo(fifo.c_str(), 0600) == 0) {
        const auto start = std::chrono::steady_clock::now();
        const CopyStatus s = CopyFileBounded(fifo.string(),
                                             (tmp.path / "dst4.bin").string(),
                                             1024, {}, &err);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        Check(s == CopyStatus::SourceNotRegular,
              std::string("fs: FIFO source rejected (") +
                  CopyStatusName(s) + ")");
        Check(ms < 1000, "fs: FIFO source check did not block");
    }

    // 符号链接源（O_NOFOLLOW）拒绝。
    const fs::path link = tmp.path / "srclink";
    std::error_code ec;
    fs::create_symlink(src, link, ec);
    if (!ec) {
        Check(CopyFileBounded(link.string(), (tmp.path / "dst5.bin").string(),
                              10, {}, &err) == CopyStatus::SourceNotRegular,
              "fs: symlinked source rejected");
    }
}

// ── 受控路径链：中间级符号链接 ───────────────────────────────────────────────

void TestCheckDirChain() {
    TempDir tmp;
    std::string bad;

    const fs::path root = tmp.path / "models";
    fs::create_directories(root / "online-zipformer/version");
    Check(CheckDirChain(root.string(), "online-zipformer/version", &bad) ==
              ChainStatus::Ok,
          "chain: real directory chain accepted");

    // 中间级换成符号链接：必须识别为 Symlink（读写会被导向受控目录之外）。
    const fs::path outside = tmp.path / "outside";
    fs::create_directories(outside / "version");
    const fs::path fakeId = root / "offline-x-asr";
    std::error_code ec;
    fs::create_directory_symlink(outside, fakeId, ec);
    if (!ec) {
        const ChainStatus status =
            CheckDirChain(root.string(), "offline-x-asr/version", &bad);
        Check(status == ChainStatus::Symlink,
              std::string("chain: intermediate symlink rejected (") +
                  ChainStatusName(status) + ")");
    }

    Check(CheckDirChain(root.string(), "missing/version", &bad) ==
              ChainStatus::Missing,
          "chain: missing component reported");

    const fs::path fileAsDir = root / "a-file";
    WriteText(fileAsDir, "x");
    Check(CheckDirChain(root.string(), "a-file/version", &bad) ==
              ChainStatus::NotDirectory,
          "chain: non-directory component rejected");

    // base 自身允许是符号链接（用户的 HOME/XDG 常如此），不检查 base。
    Check(CheckDirChain((tmp.path / "outside").string(), "version", &bad) ==
              ChainStatus::Ok,
          "chain: base itself is not treated as untrusted");
}

void TestFsyncDir() {
    TempDir tmp;
    Check(FsyncDir(tmp.path.string()), "fs: fsync existing directory");
    Check(!FsyncDir((tmp.path / "missing").string()),
          "fs: fsync missing directory fails");
    Check(!FsyncDir(""), "fs: fsync empty path fails");
}

// ── 发布不覆盖（race 回归）───────────────────────────────────────────────────

void TestRenameNoReplaceRefusesToClobber() {
    TempDir tmp;
    std::string err;

    // 正常路径：目标不存在 → 成功。
    const fs::path src = tmp.path / "src";
    fs::create_directories(src);
    WriteText(src / "payload", "new");

    const fs::path dst = tmp.path / "dst";
    const int rootFd = ::open(tmp.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    auto renameIn = [&](const std::string& from, const std::string& to) {
        return RenameNoReplaceAt(rootFd, from, rootFd, to, &err);
    };
    if (renameIn("src", "dst") != NoReplaceStatus::Ok) {
        // 文件系统不支持时不做断言，但必须明确报告而不是假装成功。
        Check(false, "fs: RENAME_NOREPLACE unsupported on this filesystem: " + err);
        return;
    }
    Check(fs::exists(dst / "payload"), "fs: no-replace publish placed content");

    // 竞态：目标在检查之后被外部创建为空目录 → 必须失败且不覆盖。
    const fs::path src2 = tmp.path / "src2";
    fs::create_directories(src2);
    WriteText(src2 / "payload", "newer");
    const fs::path dst2 = tmp.path / "dst2";
    fs::create_directories(dst2); // 外部创建（空目录）

    const NoReplaceStatus dirStatus = renameIn("src2", "dst2");
    Check(dirStatus == NoReplaceStatus::Exists,
          "fs: no-replace reports Exists when target appeared (dir)");
    Check(fs::is_directory(dst2) && fs::is_empty(dst2),
          "fs: externally created empty dir is NOT replaced");
    Check(fs::exists(src2 / "payload"),
          "fs: source untouched when publish refused");

    // 竞态：目标被外部创建为符号链接 → 必须失败且链接保持原样。
    const fs::path target = tmp.path / "external-target";
    WriteText(target, "external");
    const fs::path link = tmp.path / "dst3";
    std::error_code ec;
    fs::create_symlink(target, link, ec);
    if (!ec) {
        const NoReplaceStatus linkStatus = renameIn("src2", "dst3");
        Check(linkStatus == NoReplaceStatus::Exists,
              "fs: no-replace reports Exists when target is a symlink");
        Check(fs::is_symlink(link),
              "fs: externally created symlink is NOT replaced");
        Check(ReadFile(target) == "external",
              "fs: symlink destination content untouched");
    }

    // 目标是非空目录 → 同样拒绝。
    const fs::path dst4 = tmp.path / "dst4";
    fs::create_directories(dst4);
    WriteText(dst4 / "keep", "keep");
    Check(renameIn("src2", "dst4") == NoReplaceStatus::Exists,
          "fs: no-replace refuses non-empty target");
    Check(ReadFile(dst4 / "keep") == "keep",
          "fs: non-empty target content preserved");
    if (rootFd >= 0) ::close(rootFd);
}

// ── 有界摘要 ─────────────────────────────────────────────────────────────────

void TestDigestFileBounds() {
    TempDir tmp;

    const fs::path small = tmp.path / "small.bin";
    WriteText(small, "abc");

    const auto ok = DigestFile(small.string(), 3);
    Check(ok.status == FileDigestStatus::Ok, "digest: small file ok");
    Check(ok.size == 3, "digest: size reported");
    Check(ToHex(ok.digest) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "digest: content digest correct");

    // 上限小于文件大小 → 不读取内容，直接 TooLarge。
    const auto tooLarge = DigestFile(small.string(), 2);
    Check(tooLarge.status == FileDigestStatus::TooLarge,
          "digest: oversized file rejected before reading");

    // 巨型稀疏文件：必须立即失败而不是读 8 GiB。
    const fs::path huge = tmp.path / "huge.bin";
    {
        std::ofstream out(huge, std::ios::binary);
        out.seekp(8LL * 1024 * 1024 * 1024 - 1);
        out.put('\0');
    }
    const auto start = std::chrono::steady_clock::now();
    const auto hugeResult = DigestFile(huge.string(), 1024);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    Check(hugeResult.status == FileDigestStatus::TooLarge,
          "digest: huge sparse file rejected by fstat size");
    Check(ms < 2000, "digest: huge file rejected without reading it");

    // 取消。
    const auto cancelled = DigestFile(small.string(), 3, [] { return true; });
    Check(cancelled.status == FileDigestStatus::Cancelled,
          "digest: cancellation honoured");

    // 目录 / FIFO。
    Check(DigestFile(tmp.path.string(), 0).status ==
              FileDigestStatus::NotRegularFile,
          "digest: directory rejected");
    const fs::path fifo = tmp.path / "pipe";
    if (::mkfifo(fifo.c_str(), 0600) == 0) {
        const auto fifoResult = DigestFile(fifo.string(), 0);
        Check(fifoResult.status == FileDigestStatus::NotRegularFile,
              "digest: FIFO rejected without blocking");
    }
}

// ── 已安装校验：超大文件不得导致长时间读取 ───────────────────────────────────

void TestInstalledCheckRejectsHugeFile() {
    TempDir tmp;
    const ModelSpec* spec = FindModelSpec("offline-x-asr");
    if (!spec) return;

    const fs::path dir = tmp.path / "version";
    fs::create_directories(dir);
    for (size_t i = 0; i < spec->fileCount; ++i) {
        WriteText(dir / spec->files[i].relativePath, "x");
    }
    // 把清单里**第一个**文件换成 4 GiB 稀疏文件：校验必须靠 fstat 立即判定
    // 大小不符，而不是把 4 GiB 读完（否则该用例会明显变慢）。
    const std::string firstFile = spec->files[0].relativePath;
    const fs::path huge = dir / firstFile;
    fs::remove(huge);
    {
        std::ofstream out(huge, std::ios::binary);
        out.seekp(4LL * 1024 * 1024 * 1024 - 1);
        out.put('\0');
    }

    const auto start = std::chrono::steady_clock::now();
    const auto check = CheckInstalledModel(*spec, dir.string());
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    Check(check.state == InstalledState::Tampered,
          "installed: huge file reported tampered");
    Check(check.badFile == firstFile,
          "installed: huge file identified precisely");
    Check(ms < 2000, "installed: huge file rejected without reading it");
}

void TestInstalledCheckHonoursCancellation() {
    TempDir tmp;
    const ModelSpec* spec = FindModelSpec("offline-x-asr");
    if (!spec) return;

    const fs::path dir = tmp.path / "version";
    fs::create_directories(dir);
    for (size_t i = 0; i < spec->fileCount; ++i) {
        WriteText(dir / spec->files[i].relativePath, "x");
    }

    // 已取消：必须立即返回 cancelled，且不得报告 valid/tampered。
    const auto cancelled = CheckInstalledModel(*spec, dir.string(),
                                               [] { return true; });
    Check(cancelled.cancelled, "installed: cancellation surfaced to caller");
    Check(cancelled.state != InstalledState::Valid,
          "installed: cancelled check is never reported valid");

    // 未取消时照常工作。
    Check(!CheckInstalledModel(*spec, dir.string()).cancelled,
          "installed: normal check is not marked cancelled");
}

void TestInstalledCheckRejectsSymlinkedIdDir() {
    TempDir tmp;
    EnvGuard guard({"XDG_DATA_HOME", "HOME"});
    ::setenv("XDG_DATA_HOME", tmp.path.c_str(), 1);

    const ModelSpec* spec = FindModelSpec("offline-x-asr");
    if (!spec) return;

    // 在受控根下把 <id> 换成指向外部的符号链接，并在外部放一套"内容正确"的目录。
    const std::string root = fcitx::model_manager::DataRootDir();
    const fs::path outside = tmp.path / "outside-model";
    fs::create_directories(outside / spec->version);
    for (size_t i = 0; i < spec->fileCount; ++i) {
        WriteText(outside / spec->version / spec->files[i].relativePath, "x");
    }
    std::error_code ec;
    fs::create_directories(root);
    fs::create_directory_symlink(outside, fs::path(root) / spec->id, ec);
    if (ec) return;

    const std::string versionDir =
        fcitx::model_manager::VersionDir(spec->id, spec->version);
    const auto check = CheckInstalledModel(*spec, versionDir);
    Check(check.state == InstalledState::Unreadable,
          "installed: symlinked <id> directory rejected as unreadable");
    Check(check.detail.find("符号链接") != std::string::npos,
          "installed: symlink rejection explains the reason");
}

} // namespace

int main() {
    TestEnsureDirDoesNotChmodExisting();
    TestEnsureDirRejectsNonDirectory();
    TestEnsureDirUnderAllowsTrustedSymlinkedBase();
    TestMakeUniqueDirIsExclusive();
    TestCopyFileBounded();
    TestCheckDirChain();
    TestFsyncDir();
    TestRenameNoReplaceRefusesToClobber();
    TestDigestFileBounds();
    TestInstalledCheckRejectsHugeFile();
    TestInstalledCheckHonoursCancellation();
    TestInstalledCheckRejectsSymlinkedIdDir();

    return Report("model_manager_fs_test");
}
