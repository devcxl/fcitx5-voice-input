#pragma once

// 受控模型的落盘路径约定（唯一实现，addon 与安装器共用）。
//
// 布局：
//   <XDG_DATA_HOME>/fcitx5/voice-input/models/
//     ├── .locks/<id>.lock          # 安装互斥锁（flock）
//     ├── .staging/                 # 同文件系统暂存区
//     └── <id>/<version>/           # 不可变版本目录
//         ├── install.json          # 不可信 marker，仅用于快速否定
//         └── <relativePath...>     # 逐文件 sha256 已由清单固定
//
// <id> 与 <version> 一律来自 model_catalog（编译期常量），不接受用户输入。
// XDG_DATA_HOME 必须是绝对路径；相对值按 XDG Base Directory Spec 视为非法并忽略。

#include <string>
#include <string_view>

namespace fcitx::model_manager {

/// $XDG_DATA_HOME（仅接受绝对路径）；否则 $HOME/.local/share；
/// 都不可用时返回空串。
std::string DataHomeDir();

/// 模型根目录：DataHomeDir()/fcitx5/voice-input/models。
/// DataHomeDir() 为空时返回空串。
std::string DataRootDir();

/// <root>/<id>。
std::string ModelDir(std::string_view id);

/// <root>/<id>/<version>，即不可变版本目录。
std::string VersionDir(std::string_view id, std::string_view version);

/// <root>/.locks。
std::string LocksDir();

/// <data home>/fcitx5/voice-input —— models 的父目录。
/// 受控路径的信任边界：这一级及更上层允许是（用户的）符号链接；
/// 从 models 起必须逐级是真实目录。
std::string ModelsParentDir();

/// <root>/.staging。
std::string StagingRoot();

/// 版本目录内不可信 marker 的路径。
std::string InstallMarkerPath(const std::string& versionDir);

/// 校验型号/版本片段可以安全地作为单级路径使用：
/// 非空、长度有界、不含 '/'、'\\'、NUL、不以 '.' 开头（避免 . / .. / 隐藏名）。
bool IsSafePathSegment(std::string_view segment);

/// 校验相对文件名可以安全地拼接到版本目录下：
/// 非空、非绝对、不含 NUL、不含 ".."/"." 组件、不含空组件。
bool IsSafeRelativePath(std::string_view relativePath);

} // namespace fcitx::model_manager
