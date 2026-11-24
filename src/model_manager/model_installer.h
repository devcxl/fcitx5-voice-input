#pragma once

// 受控模型安装编排：下载 → 校验归档 → 白名单解包 → 逐文件校验 → 子进程试加载
// → 原子发布。
//
// 关键语义（对应 M4）：
//   - **同文件系统暂存**：暂存在 <root>/.staging/<id>-<pid>-<rand>，与最终版本
//     目录同属一个文件系统，因此 rename(2) 是原子的，不存在跨设备复制的窗口。
//   - **原子发布**：rename(staging, <root>/<id>/<version>)。rename 对目录要求
//     目标是空目录或不存在，所以发布前先确保目标不存在；若已存在**同内容**的
//     完整版本，则幂等收敛（保留既有目录，删掉暂存），不报错。
//   - **保留旧版本**：发布只新增目录，从不删除其它版本目录。
//   - **取消不发布半套**：任何阶段失败/取消都只删暂存目录，最终目录不会出现
//     半成品；发布之后的取消仍报告 published=true。
//   - 失败时不留暂存垃圾（除非进程被 SIGKILL，此时下次安装会清理陈旧暂存）。

#include <cstdint>
#include <functional>
#include <string>

#include "install_events.h"
#include "model_catalog.h"

namespace fcitx::model_manager {

struct InstallOptions {
    /// 非空时使用该本地归档，不联网（离线安装 / 测试）。
    std::string archivePath;
    /// 试加载限时（毫秒）。
    uint32_t trialTimeoutMs = 120000;
    /// 是否做子进程试加载（测试可关闭；生产必须开启）。
    bool runTrialLoad = true;
    /// 外部取消检查：返回 true 表示要求中止。应已包含信号标志。
    std::function<bool()> cancelled;
    /// 事件回调；为空时不输出协议。
    std::function<void(const InstallEvent&)> emit;
};

struct InstallOutcome {
    bool ok = false;
    bool published = false;
    bool cancelled = false;
    /// 已发布但父目录刷盘失败：安装成立、持久性未知（不可回滚）。
    bool durabilityUnknown = false;
    /// 已发布但发布后路径快照显示目标已不可按路径解析/已指向别处：
    /// 安装事实成立、不回滚，但**不保证该路径上的模型当前可用**。
    bool pathStateUncertain = false;
    InstallError error = InstallError::None;
    std::string detail;
    std::string versionDir;
    uint64_t elapsedMs = 0;
};

/// 安装 spec 指定的模型。
///
/// 幂等：目标版本目录已存在且逐文件校验通过时直接成功（published=false，
/// 不重复下载/解包），因此重复调用是安全的。
InstallOutcome InstallModel(const ModelSpec& spec, const InstallOptions& options);

} // namespace fcitx::model_manager
