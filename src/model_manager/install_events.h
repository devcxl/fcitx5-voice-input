#pragma once

// 安装器的 stdout 协议：**每行一个 JSON 对象**（JSON Lines），UTF-8，以 '\n' 结尾。
//
// 约定：
//   - stdout 只承载协议事件，绝不输出日志/调试信息（日志走 stderr）。
//   - 一行一个完整对象，不换行缩进，便于 GUI 逐行增量解析。
//   - 每条事件都有 "event" 字段；最终结果事件为 "result"，只出现一次且必为最后一条。
//   - 进程退出码：0 = 成功；非 0 = 失败（具体类别见 result.ok / result.error）。
//
// 事件序列（成功路径）：
//   start → (download:progress)* → (verify:archive) → (extract:progress)*
//         → (verify:file)* → (verify:trial)* → (publish) → result
//
// 取消（SIGINT/SIGTERM）：已收到的信号会触发清理；若取消发生在原子发布之后，
// result 仍报告 ok=true 且 "published": true（不得误报未安装）。

#include <cstdint>
#include <string>
#include <string_view>

#include "model_catalog.h"

namespace fcitx::model_manager {

/// 安装失败类别。序列化为稳定的字符串，供 GUI 分支处理（大小写恒为小写下划线）。
enum class InstallError {
    None = 0,
    Usage,            // 命令行参数非法
    UnknownModel,     // 不受支持的 --model
    UnsafePath,       // 暂存/版本路径非法
    LockBusy,         // 另一个安装进程持有该模型锁
    Cancelled,        // 收到 SIGINT/SIGTERM 或超时
    DownloadFailed,   // 网络/HTTP/写盘失败
    ArchiveMismatch,  // 归档 sha256 或尺寸不符
    ArchiveUnsafe,    // 归档条目越界/链接/特殊文件/超限
    ExtractFailed,    // 解包 I/O 失败
    FileMismatch,     // 解包文件 sha256/尺寸不符
    MissingFile,      // 归档缺少清单要求的文件
    TrialLoadFailed,  // 子进程试加载失败
    TrialLoadTimeout, // 子进程试加载超时
    PublishFailed,    // 原子发布失败
    Internal,         // 其他（含 OpenSSL/libarchive 初始化失败）
};

/// 稳定的字符串名（用于协议与日志）。
const char* InstallErrorName(InstallError error);

/// 人类可读的说明（中文，面向用户；不含凭据与绝对隐私路径以外的敏感信息）。
const char* InstallErrorDetail(InstallError error);

/// 进度阶段。
enum class InstallStage {
    Start,
    Download,
    VerifyArchive,
    Extract,
    VerifyFile,
    VerifyTrial,
    Publish,
    Result,
};

const char* InstallStageName(InstallStage stage);

/// 一条协议事件。字段按需填充，未使用的保持默认（不会序列化）。
struct InstallEvent {
    InstallStage stage = InstallStage::Start;

    // 通用
    std::string modelId;
    std::string version;

    // 下载
    uint64_t bytesDone = 0;
    uint64_t bytesTotal = 0;

    // 解包
    std::string file;

    // 结果
    bool ok = false;
    bool published = false;   // 版本目录已原子发布（取消后仍可能为 true）
    bool cancelled = false;   // 曾收到取消信号
    /// 已发布但父目录刷盘失败：持久性未知（发布事实成立）。
    bool durabilityUnknown = false;
    /// 已发布但目标路径状态不确定（事后快照）：不保证该路径上的模型可用。
    bool pathStateUncertain = false;
    InstallError error = InstallError::None;
    std::string detail;       // 附加诊断（可为空）
    std::string versionDir;   // 发布后的版本目录绝对路径（ok 时）
    uint64_t elapsedMs = 0;
};

/// 序列化为单行 JSON（不含换行）。始终合法，可安全写入 stdout。
std::string ToJsonLine(const InstallEvent& event);

/// 线程安全地把事件写到 stdout 并 flush。写失败（例如 GUI 关闭了管道）返回 false,
/// 调用方应视作取消信号（SIGPIPE 已忽略，不会杀掉进程）。
bool EmitEvent(const InstallEvent& event);

/// 模型状态查询结果（`--check-model <id>`）的单行 JSON：
///
///   {"event":"installed_status","model":"<id>","state":"absent|valid|tampered|unreadable",
///    "version":"<固定版本>","archiveSize":<字节数>,"source":"<固定 release URL>"}
///
/// 这是**只读**查询协议：不写目录、不联网。state 由内置清单的逐文件摘要判定；
/// version / archiveSize / source 直接取自同一份内置清单，供 GUI 展示
/// "装的应该是哪个版本、需要下载多少、来自哪里"。
std::string InstalledStatusJson(const ModelSpec& spec, std::string_view state);

/// 把上述状态行写到 stdout 并 flush。
bool EmitInstalledStatus(const ModelSpec& spec, std::string_view state);

} // namespace fcitx::model_manager
