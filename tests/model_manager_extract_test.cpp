// model_manager 单元测试（二）：白名单解包、安装锁、协议事件、安装状态校验。
//
// 归档在运行时用 libarchive 现场构造，不依赖测试数据文件，也不访问网络。

#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "archive_extract.h"
#include "install_events.h"
#include "install_lock.h"
#include "installed_model.h"
#include "model_paths.h"
#include "sha256.h"

#include "support/model_manager_test_util.h"

namespace {

using namespace model_manager_test;
using fcitx::model_manager::CheckInstalledModel;
using fcitx::model_manager::EmitEvent;
using fcitx::model_manager::ExtractStatus;
using fcitx::model_manager::ExtractStatusName;
using fcitx::model_manager::ExtractWhitelisted;
using fcitx::model_manager::ExtractedFile;
using fcitx::model_manager::FindModelSpec;
using fcitx::model_manager::InstallError;
using fcitx::model_manager::InstallErrorName;
using fcitx::model_manager::InstallEvent;
using fcitx::model_manager::InstallLock;
using fcitx::model_manager::InstalledState;
using fcitx::model_manager::InstallStage;
using fcitx::model_manager::InstalledStatusJson;
using fcitx::model_manager::InstallStageName;
using fcitx::model_manager::LockStatus;
using fcitx::model_manager::ToJsonLine;
using fcitx::model_manager::WriteInstallMarker;

// ── 白名单解包 ───────────────────────────────────────────────────────────────

void TestExtractRejectsUnsafe() {
    TempDir tmp;
    const ModelSpec* spec = FindModelSpec("offline-x-asr");
    if (!spec) return;

    struct Case {
        const char* name;
        std::vector<TarEntry> entries;
        ExtractStatus expected;
    };

    std::vector<TarEntry> traversal = MinimalEntriesFor(*spec);
    traversal.push_back({std::string(spec->version) + "/../../evil", "x", '0',
                         ""});

    std::vector<TarEntry> absolute = MinimalEntriesFor(*spec);
    absolute.push_back({"/etc/evil", "x", '0', ""});

    std::vector<TarEntry> symlink = MinimalEntriesFor(*spec);
    symlink.push_back(
        {std::string(spec->version) + "/link", "", '2', "/etc/passwd"});

    std::vector<TarEntry> hardlink = MinimalEntriesFor(*spec);
    hardlink.push_back({std::string(spec->version) + "/hard", "", '1',
                        std::string(spec->version) + "/tokens.txt"});

    std::vector<TarEntry> duplicate = MinimalEntriesFor(*spec);
    duplicate.push_back({std::string(spec->version) + "/tokens.txt", "y", '0', ""});

    std::vector<TarEntry> missing;
    missing.push_back({std::string(spec->version) + "/", "", '5', ""});
    missing.push_back({std::string(spec->version) + "/tokens.txt", "x", '0', ""});

    const Case cases[] = {
        {"traversal", traversal, ExtractStatus::UnsafeEntry},
        {"absolute", absolute, ExtractStatus::UnsafeEntry},
        {"symlink", symlink, ExtractStatus::UnsafeEntry},
        {"hardlink", hardlink, ExtractStatus::UnsafeEntry},
        {"duplicate", duplicate, ExtractStatus::UnsafeEntry},
        {"missing", missing, ExtractStatus::MissingFile},
    };

    for (const Case& c : cases) {
        const fs::path archivePath =
            tmp.path / (std::string(c.name) + ".tar.bz2");
        const fs::path dest = tmp.path / (std::string("dest-") + c.name);
        fs::create_directories(dest);
        if (!WriteTar(archivePath, c.entries)) {
            Check(false, std::string("extract: failed to build ") + c.name);
            continue;
        }
        const auto result =
            ExtractWhitelisted(archivePath.string(), *spec, dest.string(), {});
        Check(result.status == c.expected,
              std::string("extract: ") + c.name + " (got " +
                  ExtractStatusName(result.status) + ")");
        // 任何失败都不得留下白名单文件（不留"部分成功"）。
        if (c.expected != ExtractStatus::Ok) {
            bool leaked = false;
            for (size_t i = 0; i < spec->fileCount; ++i) {
                if (fs::exists(dest / spec->files[i].relativePath)) leaked = true;
            }
            Check(!leaked,
                  std::string("extract: ") + c.name + " wrote no whitelisted file");
        }
    }
}

void TestExtractIgnoresExtraRegularFiles() {
    TempDir tmp;
    const ModelSpec* spec = FindModelSpec("offline-x-asr");
    if (!spec) return;

    // 官方归档的真实形态：白名单文件之外还有 README/脚本/wav/bpe.model。
    std::vector<TarEntry> entries = MinimalEntriesFor(*spec);
    const std::string dir = std::string(spec->version) + "/";
    entries.push_back({dir + "README.md", "readme", '0', ""});
    entries.push_back({dir + "test_onnx.py", "#!/bin/sh\n", '0', ""});
    entries.push_back({dir + "bpe.model", "proto", '0', ""});
    entries.push_back({dir + "test_wavs/", "", '5', ""});
    entries.push_back({dir + "test_wavs/0.wav", "RIFF", '0', ""});
    // 另一棵顶层目录同样只是无关内容。
    entries.push_back({"other-model/tokens.txt", "q", '0', ""});

    const fs::path archivePath = tmp.path / "extras.tar.bz2";
    const fs::path dest = tmp.path / "dest-extras";
    fs::create_directories(dest);
    if (!WriteTar(archivePath, entries)) {
        Check(false, "extract: failed to build extras archive");
        return;
    }

    const auto result =
        ExtractWhitelisted(archivePath.string(), *spec, dest.string(), {});
    Check(result.status == ExtractStatus::Ok,
          "extract: extra regular files are ignored, not errors");
    Check(fs::exists(dest / "tokens.txt"), "extract: whitelisted file written");
    Check(!fs::exists(dest / "README.md"), "extract: README not written");
    Check(!fs::exists(dest / "test_onnx.py"), "extract: script not written");
    Check(!fs::exists(dest / "bpe.model"), "extract: bpe.model not written");
    Check(!fs::exists(dest / "test_wavs"), "extract: test_wavs not written");
}

void TestExtractEnforcesSizeLimit() {
    TempDir tmp;
    const ModelSpec* spec = FindModelSpec("offline-x-asr");
    if (!spec) return;

    // tokens.txt 期望 59240 字节；给 1 MiB 应触发上限而不是被写入。
    std::vector<TarEntry> entries;
    const std::string dir = std::string(spec->version) + "/";
    entries.push_back({dir, "", '5', ""});
    for (size_t i = 0; i < spec->fileCount; ++i) {
        const bool isTokens = std::strcmp(spec->files[i].role, "tokens") == 0;
        entries.push_back({dir + spec->files[i].relativePath,
                           isTokens ? std::string(1024 * 1024, 'z') : "x", '0',
                           ""});
    }

    const fs::path archivePath = tmp.path / "oversize.tar.bz2";
    const fs::path dest = tmp.path / "dest-oversize";
    fs::create_directories(dest);
    if (!WriteTar(archivePath, entries)) {
        Check(false, "extract: failed to build oversize archive");
        return;
    }
    const auto result =
        ExtractWhitelisted(archivePath.string(), *spec, dest.string(), {});
    Check(result.status == ExtractStatus::LimitExceeded,
          "extract: oversized entry rejected");
    Check(!fs::exists(dest / "tokens.txt"),
          "extract: oversized file not left behind");
}

void TestExtractCancellation() {
    TempDir tmp;
    const ModelSpec* spec = FindModelSpec("offline-x-asr");
    if (!spec) return;

    const fs::path archivePath = tmp.path / "cancel.tar.bz2";
    const fs::path dest = tmp.path / "dest-cancel";
    fs::create_directories(dest);
    if (!WriteTar(archivePath, MinimalEntriesFor(*spec))) return;

    fcitx::model_manager::ExtractCallbacks cb;
    cb.cancelled = [] { return true; };
    const auto result =
        ExtractWhitelisted(archivePath.string(), *spec, dest.string(), cb);
    Check(result.status == ExtractStatus::Cancelled,
          "extract: cancellation honoured");
}

void TestExtractUnreadableArchive() {
    TempDir tmp;
    const ModelSpec* spec = FindModelSpec("offline-x-asr");
    if (!spec) return;

    const fs::path junk = tmp.path / "junk.tar.bz2";
    WriteText(junk, "this is not an archive");
    const fs::path dest = tmp.path / "dest-junk";
    fs::create_directories(dest);

    const auto result =
        ExtractWhitelisted(junk.string(), *spec, dest.string(), {});
    Check(result.status == ExtractStatus::OpenFailed,
          "extract: non-archive rejected");
    Check(result.files.empty(), "extract: nothing recorded on failure");
}

// ── 安装锁 ───────────────────────────────────────────────────────────────────

void TestInstallLock() {
    TempDir tmp;
    const std::string lockPath = (tmp.path / "model.lock").string();

    InstallLock first;
    std::string detail;
    Check(first.TryAcquire(lockPath, &detail) == LockStatus::Acquired,
          "lock: first acquire succeeds");
    Check(first.held(), "lock: held() true after acquire");

    // flock 是"打开文件描述"级别的，同进程内另一个 fd 也能观察互斥。
    const int other = ::open(lockPath.c_str(), O_RDWR | O_CLOEXEC);
    if (other >= 0) {
        Check(::flock(other, LOCK_EX | LOCK_NB) != 0,
              "lock: second holder cannot acquire while held");
        ::close(other);
    }

    first.Release();
    Check(!first.held(), "lock: released");

    InstallLock second;
    Check(second.TryAcquire(lockPath, &detail) == LockStatus::Acquired,
          "lock: re-acquire after release");

    InstallLock bad;
    Check(bad.TryAcquire("", &detail) == LockStatus::Failed,
          "lock: empty path fails");
}

// ── 安装状态校验（addon 共用入口）─────────────────────────────────────────────

void TestCheckInstalledModelStates() {
    TempDir tmp;
    const ModelSpec* spec = FindModelSpec("offline-x-asr");
    if (!spec) return;

    // 不存在
    const fs::path dir = tmp.path / "version";
    Check(CheckInstalledModel(*spec, dir.string()).state ==
              InstalledState::Absent,
          "installed: missing dir reports absent");

    // 存在但内容错
    fs::create_directories(dir);
    for (size_t i = 0; i < spec->fileCount; ++i) {
        WriteText(dir / spec->files[i].relativePath, "wrong");
    }
    Check(CheckInstalledModel(*spec, dir.string()).state ==
              InstalledState::Tampered,
          "installed: wrong content reports tampered");

    // 符号链接冒充版本目录
    const fs::path link = tmp.path / "linkdir";
    std::error_code ec;
    fs::create_directory_symlink(dir, link, ec);
    if (!ec) {
        Check(CheckInstalledModel(*spec, link.string()).state ==
                  InstalledState::Unreadable,
              "installed: symlinked version dir reports unreadable");
    }

    // marker 不可信：伪造摘要也不影响判定（仍是 tampered）。
    Check(WriteInstallMarker(*spec, dir.string()),
          "installed: marker written");
    Check(CheckInstalledModel(*spec, dir.string()).state ==
              InstalledState::Tampered,
          "installed: marker cannot make wrong content valid");
    Check(fs::exists(dir / "install.json"), "installed: marker file exists");
}

// ── 状态查询协议（--check-model）─────────────────────────────────────────────

void TestInstalledStatusProtocol() {
    const ModelSpec* spec = FindModelSpec("offline-x-asr");
    if (!spec) return;

    const std::string line =
        InstalledStatusJson(*spec, InstalledStateName(InstalledState::Valid));
    Check(line.find("\"event\":\"installed_status\"") != std::string::npos,
          "status: event name stable");
    Check(line.find("\"model\":\"offline-x-asr\"") != std::string::npos,
          "status: model reported");
    Check(line.find("\"state\":\"valid\"") != std::string::npos,
          "status: state reported");
    // GUI 需要"应该是哪个版本 / 要下多少 / 从哪来"，全部取自内置清单。
    Check(line.find(std::string("\"version\":\"") + spec->version + "\"") !=
              std::string::npos,
          "status: version comes from catalog");
    Check(line.find("\"archiveSize\":" + std::to_string(spec->archiveSize)) !=
              std::string::npos,
          "status: archiveSize is an integer byte count");
    Check(line.find(std::string("\"source\":\"") + spec->archiveUrl + "\"") !=
              std::string::npos,
          "status: source is the pinned release URL");
    Check(std::string(spec->archiveUrl).rfind("https://", 0) == 0,
          "status: source is https");
    Check(line.find('\n') == std::string::npos, "status: single line");

    // state 变化不影响其余字段。
    const std::string absent =
        InstalledStatusJson(*spec, InstalledStateName(InstalledState::Absent));
    Check(absent.find("\"state\":\"absent\"") != std::string::npos,
          "status: absent state serialised");
    Check(absent.find(std::to_string(spec->archiveSize)) != std::string::npos,
          "status: catalog fields kept for absent state");

    // 另一个模型给出它自己的版本与体积，不会串。
    const ModelSpec* online = FindModelSpec("online-zipformer");
    if (online) {
        const std::string other =
            InstalledStatusJson(*online, InstalledStateName(InstalledState::Valid));
        Check(other.find(std::string("\"version\":\"") + online->version + "\"") !=
                  std::string::npos,
              "status: online version reported for online model");
        Check(other.find(std::to_string(online->archiveSize)) != std::string::npos,
              "status: online archiveSize reported");
    }
}

// ── 协议事件 ─────────────────────────────────────────────────────────────────

void TestEventJson() {
    InstallEvent progress;
    progress.stage = InstallStage::Download;
    progress.modelId = "offline-x-asr";
    progress.bytesDone = 1234;
    progress.bytesTotal = 5678;
    const std::string line = ToJsonLine(progress);
    Check(line.find("\"event\":\"download\"") != std::string::npos,
          "json: download event named");
    Check(line.find("\"bytesDone\":1234") != std::string::npos,
          "json: bytesDone present");
    Check(line.find("\"bytesTotal\":5678") != std::string::npos,
          "json: bytesTotal present");
    Check(line.find('\n') == std::string::npos, "json: single line");

    InstallEvent result;
    result.stage = InstallStage::Result;
    result.modelId = "offline-x-asr";
    result.ok = false;
    result.published = true;
    result.cancelled = true;
    result.error = InstallError::Cancelled;
    result.elapsedMs = 42;
    const std::string resultLine = ToJsonLine(result);
    Check(resultLine.find("\"event\":\"result\"") != std::string::npos,
          "json: result event named");
    Check(resultLine.find("\"ok\":false") != std::string::npos,
          "json: ok false serialised");
    Check(resultLine.find("\"published\":true") != std::string::npos,
          "json: published true serialised");
    Check(resultLine.find("\"cancelled\":true") != std::string::npos,
          "json: cancelled flag serialised");
    Check(resultLine.find("\"error\":\"cancelled\"") != std::string::npos,
          "json: stable error name");
    Check(resultLine.find("\"elapsedMs\":42") != std::string::npos,
          "json: elapsedMs present");

    // 事件名与错误名都是稳定的下划线小写标识（GUI 依赖它们分支）。
    for (InstallStage stage :
         {InstallStage::Start, InstallStage::Download,
          InstallStage::VerifyArchive, InstallStage::Extract,
          InstallStage::VerifyFile, InstallStage::VerifyTrial,
          InstallStage::Publish, InstallStage::Result}) {
        const std::string name = InstallStageName(stage);
        bool wellFormed = !name.empty();
        for (char c : name) {
            if (!((c >= 'a' && c <= 'z') || c == '_')) wellFormed = false;
        }
        Check(wellFormed, std::string("json: stage name stable: ") + name);
    }
    for (InstallError error :
         {InstallError::Usage, InstallError::UnknownModel,
          InstallError::UnsafePath, InstallError::LockBusy,
          InstallError::Cancelled, InstallError::DownloadFailed,
          InstallError::ArchiveMismatch, InstallError::ArchiveUnsafe,
          InstallError::ExtractFailed, InstallError::FileMismatch,
          InstallError::MissingFile, InstallError::TrialLoadFailed,
          InstallError::TrialLoadTimeout, InstallError::PublishFailed,
          InstallError::Internal}) {
        const std::string name = InstallErrorName(error);
        bool wellFormed = !name.empty();
        for (char c : name) {
            if (!((c >= 'a' && c <= 'z') || c == '_')) wellFormed = false;
        }
        Check(wellFormed, std::string("json: error name stable: ") + name);
    }
}

} // namespace

int main() {
    TestExtractRejectsUnsafe();
    TestExtractIgnoresExtraRegularFiles();
    TestExtractEnforcesSizeLimit();
    TestExtractCancellation();
    TestExtractUnreadableArchive();

    TestInstallLock();
    TestCheckInstalledModelStates();
    TestInstalledStatusProtocol();
    TestEventJson();

    return Report("model_manager_extract_test");
}
