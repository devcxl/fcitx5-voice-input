// model_manager 发布路径测试：fsync 失败、取消时机、父目录钉住、持久性未知。
//
// 这些用例通过注入 PublishIo 覆盖真实的系统调用结果，因此不依赖磁盘故障注入。

#include <chrono>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

#include "fs_safe.h"
#include "safe_remove.h"
#include "model_catalog.h"
#include "model_installer.h"
#include "model_paths.h"
#include "publish.h"

#include "support/model_manager_test_util.h"

namespace {

using namespace model_manager_test;
using fcitx::model_manager::DefaultPublishIo;
using fcitx::model_manager::FindModelSpec;
using fcitx::model_manager::InstallError;
using fcitx::model_manager::ModelsParentDir;
using fcitx::model_manager::NoReplaceStatus;
using fcitx::model_manager::OpenDirChainNoFollow;
using fcitx::model_manager::PublishIo;
using fcitx::model_manager::CheckPublishedPath;
using fcitx::model_manager::PinnedDir;
using fcitx::model_manager::PublishSource;
using fcitx::model_manager::PublishedPathState;
using fcitx::model_manager::PublishVersionDir;
using fcitx::model_manager::RenameNoReplaceAt;
using fcitx::model_manager::VersionDir;

/// 准备一个受控 XDG 根，并造出 contentDir 与目标父目录。
struct Fixture {
    TempDir tmp;
    EnvGuard guard{ {"XDG_DATA_HOME", "HOME"} };
    std::string contentDir;
    std::string finalDir;
    std::string trustedBase;
    PublishSource source;
    PinnedDir contentPin;

    explicit Fixture(const char* id) {
        ::setenv("XDG_DATA_HOME", tmp.path.c_str(), 1);
        const ModelSpec* spec = FindModelSpec(id);
        trustedBase = ModelsParentDir();
        if (!spec) return;
        finalDir = VersionDir(spec->id, spec->version);
        // 父目录（<root>/<id>）必须存在；content 在暂存区。
        const size_t slash = finalDir.find_last_of('/');
        if (slash != std::string::npos) {
            fs::create_directories(finalDir.substr(0, slash));
        }
        // 源目录放在受控根下的 .staging 里，与真实安装一致。
        const std::string stagingRoot =
            fcitx::model_manager::StagingRoot();
        fs::create_directories(stagingRoot);
        contentDir = (fs::path(stagingRoot) / "run-XXXX/content").string();
        fs::create_directories(contentDir);
        WriteText(fs::path(contentDir) / "tokens.txt", "payload");

        std::string err;
        source.dir = contentDir;
        source.relativeToModelsParent = "models/.staging/run-XXXX/content";
        contentPin = PinnedDir::Open(contentDir, &err);
        source.pinnedFd = contentPin.fd();
    }
};

/// 刷盘目标快照（在 fsync 当时取，fd 之后会被关闭）。
struct FsyncTarget {
    bool valid = false;
    bool isDir = false;
    dev_t dev = 0;
    ino_t ino = 0;
};

/// 只注入 fsync（fd 版本）；发布是否发生用文件系统观察，避免用桩替代真实 rename。
/// targets 记录每次刷盘对象的身份，便于断言"刷的是钉住的那个目录"。
PublishIo FsyncIo(int* fsyncCalls, bool failContentFsync, bool failParentFsync,
                  std::vector<FsyncTarget>* targets = nullptr) {
    PublishIo io;
    io.fsyncFd = [=](int fd) {
        const int call = ++(*fsyncCalls);
        if (targets) {
            FsyncTarget t;
            struct stat st {};
            if (fd >= 0 && ::fstat(fd, &st) == 0) {
                t.valid = true;
                t.isDir = S_ISDIR(st.st_mode);
                t.dev = st.st_dev;
                t.ino = st.st_ino;
            }
            targets->push_back(t);
        }
        if (call == 1 && failContentFsync) return false;  // 内容目录刷盘失败
        if (call == 2 && failParentFsync) return false;   // 父目录刷盘失败
        return ::fsync(fd) == 0;  // 真实刷盘，避免测试假装成功
    };
    return io;
}

// ── 发布前 fsync 失败：不得发布 ──────────────────────────────────────────────

void TestContentFsyncFailureBlocksPublish() {
    Fixture fx("offline-x-asr");
    const ModelSpec* spec = FindModelSpec("offline-x-asr");
    if (!spec || fx.finalDir.empty()) return;

    int fsyncCalls = 0;
    const auto out = PublishVersionDir(
        *spec, fx.source, fx.finalDir, /*finalExistedUpFront=*/false, {},
        FsyncIo(&fsyncCalls, /*failContentFsync=*/true,
                /*failParentFsync=*/false));

    Check(!out.ok, "publish: failed fsync must not report success");
    Check(!out.published, "publish: failed fsync must not publish");
    Check(out.error == InstallError::PublishFailed,
          "publish: failed fsync reports publish_failed");
    Check(!fs::exists(fx.finalDir),
          "publish: version dir is NOT created after fsync failure");
    Check(out.detail.find("刷盘") != std::string::npos,
          "publish: failure explains the fsync problem");
}

// ── fsync 之后、rename 之前的取消：必须拦住 ──────────────────────────────────

void TestCancelAfterFsyncBlocksRename() {
    Fixture fx("offline-x-asr");
    const ModelSpec* spec = FindModelSpec("offline-x-asr");
    if (!spec || fx.finalDir.empty()) return;

    int fsyncCalls = 0;
    bool cancelRequested = false;

    PublishIo io = FsyncIo(&fsyncCalls, false, false);
    // 模拟"fsync 期间收到取消"：内容目录刷盘完成后才置位。
    const auto counting = io.fsyncFd;
    io.fsyncFd = [&, counting](int fd) {
        const bool okResult = counting(fd);
        if (fsyncCalls == 1) cancelRequested = true;
        return okResult;
    };

    const auto out = PublishVersionDir(
        *spec, fx.source, fx.finalDir, /*finalExistedUpFront=*/false,
        [&] { return cancelRequested; }, io);

    Check(out.error == InstallError::Cancelled,
          "publish: cancellation between fsync and rename is honoured");
    Check(!out.published, "publish: cancelled run did not publish");
    Check(!fs::exists(fx.finalDir),
          "publish: no version dir created when cancelled after fsync");
    Check(fsyncCalls == 1,
          "publish: content fsync happened before the cancellation point");
}

// ── 发布后父目录 fsync 失败：已发布，但持久性未知 ────────────────────────────

void TestParentFsyncFailureReportsDurabilityUnknown() {
    Fixture fx("offline-x-asr");
    const ModelSpec* spec = FindModelSpec("offline-x-asr");
    if (!spec || fx.finalDir.empty()) return;

    int fsyncCalls = 0;
    const auto out = PublishVersionDir(
        *spec, fx.source, fx.finalDir, /*finalExistedUpFront=*/false, {},
        FsyncIo(&fsyncCalls, /*failContentFsync=*/false,
                /*failParentFsync=*/true));

    Check(out.ok, "publish: parent fsync failure does not fail the install");
    Check(out.published, "publish: parent fsync failure still counts as published");
    Check(out.durabilityUnknown,
          "publish: parent fsync failure flagged as durability unknown");
    Check(fs::exists(fs::path(fx.finalDir) / "tokens.txt"),
          "publish: content really moved into place");
    Check(out.detail.find("持久") != std::string::npos,
          "publish: detail states durability is unknown");
}

// ── 受控父目录被换成符号链接：拒绝发布 ──────────────────────────────────────

void TestSymlinkedTargetParentIsRefused() {
    TempDir tmp;
    EnvGuard guard({"XDG_DATA_HOME", "HOME"});
    ::setenv("XDG_DATA_HOME", tmp.path.c_str(), 1);

    const ModelSpec* spec = FindModelSpec("offline-x-asr");
    if (!spec) return;

    const std::string finalDir = VersionDir(spec->id, spec->version);
    const std::string root = fcitx::model_manager::DataRootDir();
    fs::create_directories(root);

    // <root>/<id> 指向外部目录（模拟校验后被替换成符号链接）。
    const fs::path outside = tmp.path / "outside";
    fs::create_directories(outside);
    std::error_code ec;
    fs::create_directory_symlink(outside, fs::path(root) / spec->id, ec);
    if (ec) return;

    const std::string contentDir =
        std::string(fcitx::model_manager::StagingRoot()) + "/run-XXXX/content";
    fs::create_directories(contentDir);
    WriteText(fs::path(contentDir) / "tokens.txt", "payload");
    std::string idErr;
    const PinnedDir srcPin = PinnedDir::Open(contentDir, &idErr);
    PublishSource src;
    src.dir = contentDir;
    src.relativeToModelsParent = "models/.staging/run-XXXX/content";
    src.pinnedFd = srcPin.fd();

    int fsyncCalls = 0;
    const auto out = PublishVersionDir(
        *spec, src, finalDir, /*finalExistedUpFront=*/false, {},
        FsyncIo(&fsyncCalls, false, false));

    Check(!out.ok, "publish: symlinked parent dir refuses to publish");
    Check(!out.published, "publish: symlinked parent dir published nothing");

    Check(!fs::exists(outside / spec->version),
          "publish: nothing written through the symlinked parent");

    // fd 固定本身也要能识别符号链接。
    const auto opened =
        OpenDirChainNoFollow(fcitx::model_manager::ModelsParentDir(),
                             std::string("models/") + spec->id);
    Check(opened.fd < 0, "publish: OpenDirChainNoFollow rejects symlink");
    if (opened.fd >= 0) ::close(opened.fd);
}

// ── 父目录在"打开 fd 之后"被搬走/替换：rename 仍写入原目录 ─────────────────

void TestPinnedParentSurvivesExternalMove() {
    TempDir tmp;
    std::string err;

    const fs::path base = tmp.path / "base";
    const fs::path pinned = base / "models" / "id";
    fs::create_directories(pinned);

    // 1) 钉住父目录（模拟 PublishVersionDir 的 OpenDirChainNoFollow）。
    const auto opened = OpenDirChainNoFollow(base.string(), "models/id");
    Check(opened.fd >= 0, "pin: parent directory opened via chain");
    if (opened.fd < 0) return;

    // 2) 外部把该目录搬走，并在原路径放一个指向外部的符号链接。
    const fs::path moved = base / "models" / "id.moved";
    const fs::path outside = tmp.path / "outside";
    fs::create_directories(outside);
    std::error_code ec;
    fs::rename(pinned, moved, ec);
    Check(!ec, "pin: external move simulated");
    fs::create_directory_symlink(outside, pinned, ec);

    // 3) 用 fd 执行 rename：应落到被搬走的那个目录里，绝不穿过符号链接。
    const fs::path src = tmp.path / "content";
    fs::create_directories(src);
    WriteText(src / "tokens.txt", "payload");

    const std::string leaf = "version";
    const int srcParent =
        ::open(tmp.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const auto status = fcitx::model_manager::RenameNoReplaceAt(
        srcParent, "content", opened.fd, leaf, &err);
    if (srcParent >= 0) ::close(srcParent);
    ::close(opened.fd);

    Check(status == fcitx::model_manager::NoReplaceStatus::Ok,
          std::string("pin: renameat2 via fd succeeded (") +
              fcitx::model_manager::NoReplaceStatusName(status) + ")");
    Check(fs::exists(moved / leaf / "tokens.txt"),
          "pin: content landed in the pinned (moved) directory");
    Check(!fs::exists(outside / leaf),
          "pin: nothing written through the replacement symlink");
}

// ── 源目录被替换：拒绝发布 ──────────────────────────────────────────────────

void TestReplacedSourceDirectoryIsRefused() {
    Fixture fx("offline-x-asr");
    const ModelSpec* spec = FindModelSpec("offline-x-asr");
    if (!spec || fx.finalDir.empty()) return;

    // 钉住 fd 后，把源目录换掉（模拟同用户在暂存期替换内容）。
    const std::string moved = fx.contentDir + ".moved";
    std::error_code ec;
    fs::rename(fx.contentDir, moved, ec);
    Check(!ec, "source: original content moved away (simulated)");
    fs::create_directories(fx.contentDir);
    WriteText(fs::path(fx.contentDir) / "tokens.txt", "attacker-content");

    int fsyncCalls = 0;
    const auto out = PublishVersionDir(*spec, fx.source, fx.finalDir,
                                       /*finalExistedUpFront=*/false, {},
                                       FsyncIo(&fsyncCalls, false, false));

    Check(!out.ok, "source: replaced source directory refuses publish");
    Check(!out.published, "source: nothing published from replaced source");
    Check(out.detail.find("身份") != std::string::npos,
          "source: refusal explains identity mismatch");
    Check(!fs::exists(fs::path(fx.finalDir) / "tokens.txt"),
          "source: version dir not created from replaced source");
}

// ── 发布后路径快照：父目录被搬走后必须报告 Uncertain ────────────────────────

void TestPublishedPathStateDetectsMovedParent() {
    TempDir tmp;
    std::string err;

    // 受控结构：<base>/models/<id>/{version}
    const fs::path base = tmp.path / "base";
    const fs::path idDir = base / "models/id";
    fs::create_directories(idDir);
    const fs::path content = tmp.path / "content";
    fs::create_directories(content);
    WriteText(content / "tokens.txt", "payload");

    const auto parent = OpenDirChainNoFollow(base.string(), "models/id");
    Check(parent.fd >= 0, "pathcheck: parent fd pinned");
    if (parent.fd < 0) return;

    const std::string leaf = "version";
    const std::string finalDir = (idDir / leaf).string();
    const auto status =
        RenameNoReplaceAt(::open(tmp.path.c_str(), O_RDONLY | O_DIRECTORY),
                          "content", parent.fd, leaf, &err);
    // 上面那行只是把 content 移进 id 目录；fd 立即用不上就关掉。
    Check(status == fcitx::model_manager::NoReplaceStatus::Ok,
          "pathcheck: content placed via pinned parent");

    // 正常：快照 Ok。
    Check(CheckPublishedPath(parent.fd, leaf, finalDir) ==
              PublishedPathState::Ok,
          "pathcheck: consistent parent reports Ok");

    // 父目录被搬走：路径解析不到 or 指向别处 → Uncertain。
    const fs::path moved = base / "models/id.moved";
    std::error_code ec;
    fs::rename(idDir, moved, ec);
    Check(!ec, "pathcheck: parent moved away (simulated)");

    Check(CheckPublishedPath(parent.fd, leaf, finalDir) ==
              PublishedPathState::Uncertain,
          "pathcheck: moved parent reports Uncertain (no rollback claim)");

    // 父目录虽被搬走，但内容确实还在原 inode 下（发布事实成立）。
    Check(fs::exists(moved / leaf / "tokens.txt"),
          "pathcheck: published content still present at the moved parent");

    ::close(parent.fd);
}

// ── 正常路径：内容与父目录都刷盘成功 ────────────────────────────────────────

void TestSuccessfulPublishHasNoDurabilityWarning() {
    Fixture fx("offline-x-asr");
    const ModelSpec* spec = FindModelSpec("offline-x-asr");
    if (!spec || fx.finalDir.empty()) return;

    int fsyncCalls = 0;
    const auto out = PublishVersionDir(
        *spec, fx.source, fx.finalDir, /*finalExistedUpFront=*/false, {},
        FsyncIo(&fsyncCalls, false, false));

    Check(out.ok && out.published, "publish: happy path publishes");
    Check(!out.durabilityUnknown, "publish: happy path has no durability warning");
    Check(!out.pathStateUncertain,
          "publish: happy path reports a consistent path");
    Check(out.detail == "已发布", "publish: happy path detail is plain");
    Check(fsyncCalls == 2, "publish: files and parent dir both fsynced");
    Check(fs::exists(fs::path(fx.finalDir) / "tokens.txt"),
          "publish: content is in the version dir");

    // 第二次刷盘必须是**已钉住的父目录 fd**，而不是按路径重新解析的目录。
    std::vector<FsyncTarget> seen;
    int calls2 = 0;
    Fixture fx2("online-zipformer");
    const ModelSpec* online = FindModelSpec("online-zipformer");
    if (online && !fx2.finalDir.empty()) {
        const auto out2 = PublishVersionDir(
            *online, fx2.source, fx2.finalDir, /*finalExistedUpFront=*/false,
            {}, FsyncIo(&calls2, false, false, &seen));
        Check(out2.ok && seen.size() == 2,
              "publish: second fsync saw a directory fd");
        if (seen.size() == 2) {
            struct stat pathSt {};
            const std::string parent =
                std::string(fx2.finalDir, 0, fx2.finalDir.find_last_of('/'));
            const bool pathOk = ::stat(parent.c_str(), &pathSt) == 0;
            Check(seen[1].valid && pathOk && seen[1].ino == pathSt.st_ino &&
                      seen[1].dev == pathSt.st_dev,
                  "publish: parent fsync targets the pinned directory inode");
            Check(seen[1].isDir,
                  "publish: pinned fsync target is a directory");
        }
    }
}

} // namespace

int main() {
    TestContentFsyncFailureBlocksPublish();
    TestCancelAfterFsyncBlocksRename();
    TestParentFsyncFailureReportsDurabilityUnknown();
    TestSymlinkedTargetParentIsRefused();
    TestPinnedParentSurvivesExternalMove();
    TestReplacedSourceDirectoryIsRefused();
    TestPublishedPathStateDetectsMovedParent();
    TestSuccessfulPublishHasNoDurabilityWarning();

    return Report("model_manager_publish_test");
}
