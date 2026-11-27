#pragma once

// 版本目录的原子发布（安装器的最后一步，也是最需要谨慎的一步）。
//
// 硬约束：
//   1. **不覆盖、不移动已有目录**：目标已存在时拒绝发布。无论是安装期间被外部
//      创建的符号链接/空目录，还是安装开始时就存在的、未通过校验的旧目录，
//      都不自动替换（EXCHANGE 会无条件把当时在位的对象挪走，无法证明期间未被
//      外部换过），两者都提示用户人工处理。
//   2. **绝不假装原子**：只用 `renameat2(RENAME_NOREPLACE)`；文件系统不支持时
//      显式失败，不退化成裸 `rename`。
//   3. **不删除用户资产**：发布只做"放置"动作，不移动/删除任何既有目录。
//
// 原子性范围：`RENAME_NOREPLACE` 提供的是**命名空间层面的原子性**（读者要么
// 看不到版本目录、要么看到完整目录），保证进程中断/并发下不会出现半套目录。
// 它不承诺断电后的持久性；持久化需要调用方对文件与目录 fsync（本模块会在发布
// 前后 fsync contentDir 与父目录）。

#include <functional>
#include <string>

#include "fs_safe.h"
#include "safe_remove.h"
#include "install_events.h"
#include "model_catalog.h"

namespace fcitx::model_manager {

struct PublishOutcome {
    bool ok = false;
    /// true 表示新内容已成为版本目录内容（调用方据此报告"已安装"）。
    bool published = false;
    /// true 表示目标已是校验通过的目录，本次未替换（幂等收敛）。
    bool converged = false;
    /// true 表示已发布但父目录刷盘失败：**发布事实成立、持久性未知**，
    /// 调用方必须按"已安装"报告，同时把持久性未知告知用户（不可回滚）。
    bool durabilityUnknown = false;
    /// true 表示已发布，但**发布后**按路径回查发现目标已不是我们刚放入的那个
    /// 目录（父目录被搬走/替换等）：发布事实成立、不可回滚，但**不得**声称
    /// 该路径上的模型当前可用。
    ///
    /// 这是尽力而为的**事后快照**，不是保证：同 UID 进程可在快照之后继续搬动。
    bool pathStateUncertain = false;
    InstallError error = InstallError::None;
    std::string detail;
};

/// 发布过程中可注入的 IO（默认走真实实现；测试用它覆盖 fsync 失败路径）。
///
/// 只注入 fsync：重命名必须走 fd 版本（`RenameNoReplaceAt`），注入一个路径版
/// 桩会掩盖真正的实现，反而降低测试价值。
struct PublishIo {
    /// 对**已打开的目录 fd** 刷盘（不是路径）。返回 false 表示失败。
    ///
    /// 用 fd 而非路径：发布需要刷的是已经钉住的那个目录，按路径重新解析可能
    /// 指向被外部替换后的另一个目录，从而错误声称"已刷盘"。
    std::function<bool(int fd)> fsyncFd;
};

/// 发布源（本次解包产物）的描述。
///
/// 只给路径是不够的：源目录的父级被搬走/替换后，按路径 rename 可能把**别的**
/// 目录移进模型路径并误报成功。因此同时给出：
///   - 相对受控根（ModelsParentDir()）的路径，供逐级 `openat(O_NOFOLLOW)` 定位；
///   - 创建时钉住的目录 fd（调用方保持打开），发布前用 fstat 与该 fd 比对。
///
/// 为什么是 fd 而不是记录的 dev/ino 值：目录一旦被删除，inode 会被文件系统回收
/// 复用；只用历史数值比较，无法区分原目录与复用同一 inode 的新目录。持有 fd
/// 可阻止复用，使比较可靠。
struct PublishSource {
    /// 绝对路径，仅用于诊断信息。
    std::string dir;
    /// 相对 ModelsParentDir() 的路径，例如 "models/.staging/<name>/content"。
    std::string relativeToModelsParent;
    /// 创建该目录时钉住的 fd；<0 表示未知 → 拒绝发布。调用方保证其生命周期。
    int pinnedFd = -1;
};

/// 默认 IO（真实系统调用）。
PublishIo DefaultPublishIo();

/// 发布后路径一致性快照（best-effort）。
enum class PublishedPathState {
    Ok = 0,      // 父目录 fd 下的 <leaf> 与路径解析到同一 inode
    Uncertain,   // 路径已不可解析，或指向另一个 inode（父目录被搬走/替换）
};

const char* PublishedPathStateName(PublishedPathState state);

/// 比较"已钉住父目录下的子项"与"按路径解析的 finalDir"是否为同一 inode。
///
/// 调用方必须传入**发布时实际使用的** parentFd；本函数只做快照，不做修复。
/// 同 UID 进程可在本函数返回后继续搬动目录，因此结果不代表后续可用性。
PublishedPathState CheckPublishedPath(int parentFd, const std::string& leaf,
                                     const std::string& finalDir);

/// 把 contentDir（本次暂存的解包产物）发布为 finalDir（版本目录）。
///
/// finalExistedUpFront 来自安装开始时的快照，仅用于区分提示语：
///   - 当时不存在、现在存在 → "安装期间被外部创建，已拒绝覆盖"；
///   - 当时已存在         → "已存在未通过校验的版本目录，请手动移除后重试"。
/// 两种情况**都不做替换**。
///
/// 取消语义：fsync 成功后、rename 之前检查取消；一旦 rename 成功即视为已发布。
///
/// fsync 语义：
///   - 发布**前**对 contentDir 刷盘失败 → 直接失败且**不发布**（不会出现
///     "发布了未刷盘内容"的情况）；
///   - 发布后对父目录刷盘失败 → 发布**已经发生、不可回滚**，返回
///     `ok=true, published=true, durabilityUnknown=true`，由调用方如实报告
///     "已安装但持久性未知"，不谎称未安装。
PublishOutcome PublishVersionDir(const ModelSpec& spec,
                                 const PublishSource& source,
                                 const std::string& finalDir,
                                 bool finalExistedUpFront,
                                 const std::function<bool()>& cancelled = {},
                                 const PublishIo& io = {});

} // namespace fcitx::model_manager
