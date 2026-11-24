#pragma once

// 安装器使用的最小文件系统原语（每个都带明确的安全语义）。
//
// 设计原则：
//   - **独占创建**：目录/文件创建必须能区分"我创建的"与"已存在的"，
//     据此决定是否设置权限，绝不对已存在的用户目录 chmod。
//   - **有界复制**：从不信任路径属性，打开后以 fd 上的 fstat 判定类型与大小。
//   - **不阻塞**：所有打开都带 O_NONBLOCK，FIFO/设备不会卡住调用方。

#include <cstdint>
#include <functional>
#include <string>

namespace fcitx::model_manager {

/// 确保目录存在。
///
/// 语义：
///   - 目录不存在 → 创建并设为 mode；
///   - 已存在且是**真实目录** → 成功，但**不修改其权限**（可能是用户自己的目录）；
///   - 已存在但不是目录（含符号链接）→ 失败。
///
/// 返回 false 时 err 给出诊断。
bool EnsureDir(const std::string& path, unsigned mode, std::string* err);

/// 同 EnsureDir，但 **trustedBase 及其上层允许是符号链接**（用户 HOME/XDG 常见），
/// 只有 trustedBase 之下的受控段才要求逐级真实目录。
///
/// 例：trustedBase = `<data home>/fcitx5/voice-input` 时，`models/<id>/<version>`
/// 三级会被严格检查，而 `fcitx5`、`voice-input` 以及更上层可以是系统符号链接。
bool EnsureDirUnder(const std::string& path, const std::string& trustedBase,
                    unsigned mode, std::string* err);

/// 在 root 下创建一个**独占**的临时目录（mkdtemp 语义，0700）。
/// label 仅用于可读性前缀（会被清洗）。
/// 返回目录绝对路径；失败返回空串并写 err。
std::string MakeUniqueDir(const std::string& root, const std::string& label,
                          std::string* err);

/// 有界复制的结果分类（供调用方映射为准确的对用户错误）。
enum class CopyStatus {
    Ok = 0,
    Cancelled,
    SourceNotRegular,  // 源是目录/FIFO/设备/符号链接
    SourceTooLarge,    // 源超过上限（未读取内容或读取中即中止）
    IoError,           // 打开/读写/刷盘失败
};

const char* CopyStatusName(CopyStatus status);

/// 把 src 的内容复制到新建的 dst（O_CREAT|O_EXCL，不允许已存在），
/// 复制过程中边读边校验上限与取消。
///
/// 用途：离线归档在**校验前**先复制到私有暂存，避免"校验后按路径重开"
/// 的 TOCTOU（外部可在两次 open 之间替换文件）。
///
/// 已写出的 dst 由调用方目录整体清理。
CopyStatus CopyFileBounded(const std::string& src, const std::string& dst,
                           uint64_t maxBytes,
                           const std::function<bool()>& cancelled,
                           std::string* err);

/// 受控路径链的校验结果。
enum class ChainStatus {
    Ok = 0,
    Missing,      // 某一级不存在
    Symlink,      // 某一级是符号链接（可能逃出受控目录）
    NotDirectory, // 某一级存在但不是目录
    IoError,      // 其他 lstat 失败
};

const char* ChainStatusName(ChainStatus status);

/// 校验 base/relative 的**每一级目录**都是真实目录（拒绝符号链接）。
///
/// base 自身不检查：用户的 HOME/XDG 常常经过符号链接，那是用户自己的信任范围。
/// 只约束 base 之下的受控段（如 <models>/<id>/<version>），防止中间级符号链接
/// 把读写导向受控目录之外。
ChainStatus CheckDirChain(const std::string& base, const std::string& relative,
                          std::string* badComponent);

/// fsync 一个目录（使其中的条目变更落盘）。失败返回 false。
bool FsyncDir(const std::string& path);

/// 发布用的"不覆盖"重命名（renameat2 RENAME_NOREPLACE）。
///
/// 与裸 rename(2) 的关键区别：目标已存在时**失败而不是替换**。这消除了
/// "检查时目标不存在 → 发布时目标已被外部创建"的竞态：外部程序在此期间
/// 放下的符号链接/空目录不会被静默覆盖（flock 不约束外部程序）。
///
/// 返回分类，调用方据此决定是否继续（例如目标已存在时改走交换或直接失败）。
enum class NoReplaceStatus {
    Ok = 0,
    Exists,     // 目标已存在（EEXIST），未做任何修改
    Unsupported,// 内核/文件系统不支持 RENAME_NOREPLACE（EINVAL/ENOSYS）
    IoError,    // 其他失败
};

const char* NoReplaceStatusName(NoReplaceStatus status);

/// 以 fd 固定路径链：从 trustedBase 起逐级 `openat(O_DIRECTORY|O_NOFOLLOW)`。
///
/// 用途：发布前把目标父目录"钉住"，避免 lstat 检查之后父路径被换成符号链接，
/// 导致 rename 被导向受控目录之外。返回的 fd 由调用方关闭；相对路径中的每一级
/// 都拒绝符号链接。`trustedBase` 自身允许是符号链接（用户 HOME/XDG 常见）。
///
/// 说明：这消除了"目标父目录被替换成符号链接"的窗口，但**不**声称消除所有
/// 路径竞态（例如攻击者把某级换成另一个真实目录）。
struct OpenDirChainResult {
    int fd = -1;
    ChainStatus status = ChainStatus::Ok;
    std::string badComponent;
};

OpenDirChainResult OpenDirChainNoFollow(const std::string& trustedBase,
                                        const std::string& relative);

/// 两端都用 fd 的"不覆盖"重命名：
///   renameat2(srcDirFd, srcName, dstDirFd, dstName, RENAME_NOREPLACE)
///
/// 源也必须是 fd：若源按路径传入，其父目录被搬走/替换后可能把**别的**目录
/// 移进模型路径并误报成功。
NoReplaceStatus RenameNoReplaceAt(int srcDirFd, const std::string& srcName,
                                  int dstDirFd, const std::string& dstName,
                                  std::string* err);



} // namespace fcitx::model_manager
