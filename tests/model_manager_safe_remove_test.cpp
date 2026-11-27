// model_manager 安全清理测试：目录身份、inode 复用、符号链接不跟随。
//
// 从 model_manager_publish_test.cpp 拆出，保持每个测试文件 < 500 行。
// 所有清理只作用于本用例自己创建的临时目录。

#include <string>

#include "safe_remove.h"

#include "support/model_manager_test_util.h"

namespace {

using namespace model_manager_test;
using fcitx::model_manager::DirIdentityOfPath;
using fcitx::model_manager::PinnedDir;
using fcitx::model_manager::RemoveTreePinned;

// ── 清理竞态：staging 被换成符号链接时不得删除外部资产 ──────────────────────

void TestPinnedRemovalRefusesReplacedDirectory() {
    TempDir tmp;
    std::string err;

    // 受控根：<tmp>/data/fcitx5/voice-input/models/.staging
    const fs::path base = tmp.path / "data/fcitx5/voice-input";
    const fs::path staging = base / "models/.staging";
    const fs::path mine = staging / "run-AAAA";
    fs::create_directories(mine / "content");
    WriteText(mine / "content/tokens.txt", "mine");

    const std::string relative = "models/.staging/run-AAAA";
    PinnedDir minePin = PinnedDir::Open(mine.string(), &err);
    Check(minePin.valid(), "cleanup: staging directory pinned (fd held)");

    // 1) 正常情况：仍是同一个目录 → 删除成功。
    Check(RemoveTreePinned(base.string(), relative, minePin.fd(), &err),
          "cleanup: pinned removal succeeds for own directory");
    Check(!fs::exists(mine), "cleanup: own directory removed");

    // 2) 竞态：同名目录被外部重建（身份不同）→ 必须保留。
    fs::create_directories(mine / "content");
    WriteText(mine / "content/other.txt", "external-asset");
    std::string err2;
    Check(!RemoveTreePinned(base.string(), relative, minePin.fd(), &err2),
          "cleanup: replaced directory is NOT deleted");
    Check(fs::exists(mine / "content/other.txt"),
          "cleanup: external asset inside replacement kept intact");

    // 3) 竞态：.staging 被换成指向外部的符号链接 → 必须保留外部内容。
    const fs::path outside = tmp.path / "outside";
    fs::create_directories(outside / "run-AAAA/content");
    WriteText(outside / "run-AAAA/content/precious.txt", "outside-asset");

    std::error_code ec;
    fs::remove_all(staging, ec);            // 仅删本次测试自己的 staging
    fs::create_directory_symlink(outside, staging, ec);
    Check(!ec, "cleanup: staging replaced by symlink (simulated)");

    std::string err3;
    Check(!RemoveTreePinned(base.string(), relative, minePin.fd(), &err3),
          "cleanup: symlinked staging refuses removal");
    Check(fs::exists(outside / "run-AAAA/content/precious.txt"),
          "cleanup: nothing deleted through the symlinked staging");
}

// ── inode 复用回归：钉住 fd 期间，新目录不可能复用同一 inode ────────────────

void TestPinnedFdPreventsInodeReuse() {
    TempDir tmp;
    std::string err;

    const fs::path dir = tmp.path / "subject";
    fs::create_directories(dir);
    const PinnedDir pin = PinnedDir::Open(dir.string(), &err);
    Check(pin.valid(), "inode-reuse: directory pinned");
    if (!pin.valid()) return;

    // 删除路径后立即反复创建同名目录：只要钉住 fd 还在，inode 就不能被回收，
    // 因此任何新目录都不会复用旧 inode（这正是不基于 fd 的值比较会失败之处）。
    std::error_code ec;
    fs::remove_all(dir, ec);

    bool sawSameInode = false;
    for (int i = 0; i < 64; ++i) {
        fs::create_directories(dir, ec);
        if (ec) break;
        const DirIdentity fresh = DirIdentityOfPath(dir.string(), &err);
        if (fresh.valid && fresh.dev == pin.identity().dev &&
            fresh.ino == pin.identity().ino) {
            sawSameInode = true;
            break;
        }
        fs::remove_all(dir, ec);
    }
    Check(!sawSameInode,
          "inode-reuse: pinned fd keeps the inode from being reused");

    // 且此时用钉住 fd 判身份：同名新目录必被判定为"不是本次创建的那个"。
    fs::create_directories(dir, ec);
    Check(!RemoveTreePinned(tmp.path.string(), "subject", pin.fd(), &err),
          "inode-reuse: recreated directory is refused deletion");
    Check(fs::exists(dir), "inode-reuse: recreated directory kept");
}

// ── 清理：删除不跟随符号链接 ────────────────────────────────────────────────

void TestPinnedRemovalDoesNotFollowSymlinks() {
    TempDir tmp;
    std::string err;

    const fs::path base = tmp.path / "data/fcitx5/voice-input";
    const fs::path mine = base / "models/.staging/run-BBBB";
    const fs::path outside = tmp.path / "outside";
    fs::create_directories(mine / "content");
    fs::create_directories(outside);
    WriteText(outside / "precious.txt", "outside-asset");
    WriteText(mine / "content/tokens.txt", "mine");
    std::error_code ec;
    fs::create_directory_symlink(outside, mine / "link-to-outside", ec);
    if (ec) return;

    const PinnedDir minePin = PinnedDir::Open(mine.string(), &err);
    Check(minePin.valid(), "cleanup: directory pinned for symlink case");

    Check(RemoveTreePinned(base.string(), "models/.staging/run-BBBB",
                           minePin.fd(), &err),
          "cleanup: removal succeeds with embedded symlink");
    Check(!fs::exists(mine), "cleanup: directory tree removed");
    Check(fs::exists(outside / "precious.txt"),
          "cleanup: symlink target content untouched");
}

} // namespace

int main() {
    TestPinnedRemovalRefusesReplacedDirectory();
    TestPinnedFdPreventsInodeReuse();
    TestPinnedRemovalDoesNotFollowSymlinks();

    return Report("model_manager_safe_remove_test");
}
