/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#ifndef _MODEL_GUI_INSTALLER_PROTOCOL_H_
#define _MODEL_GUI_INSTALLER_PROTOCOL_H_

#include <QMetaType>
#include <QString>

namespace fcitx::voiceinput {

/// 安装器（src/model_manager）stdout JSON Lines 的事件类型。
/// 与 src/model_manager/install_events.h 的 InstallStage 一一对应。
enum class InstallerEventType {
    Start,
    Download,
    VerifyArchive,
    Extract,
    VerifyFile,
    VerifyTrial,
    Publish,
    Result,
    Log,     ///< 无法解析的行（安装器日志应走 stderr，这里只兜底）
};

/// 安装状态检查（`--check-model <id>`）的结果。
/// 安装器输出一行：
///   {"event":"installed_status","model":"<id>",
///    "state":"absent|valid|tampered|unreadable"}
enum class ModelCheckState {
    Unknown,   ///< 解析失败/未返回/取值未知
    Absent,    ///< 未安装
    Valid,     ///< 已按内置清单摘要校验通过
    Tampered,  ///< 文件存在但与清单摘要/尺寸不符
    Unreadable,///< 存在但无法读取/权限/IO 错误
};

QString ModelCheckStateToken(ModelCheckState state);

struct ModelCheckEvent {
    bool recognized = false; ///< 是否是合法的 installed_status 行
    QString model;
    ModelCheckState state = ModelCheckState::Unknown;
    QString rawState; ///< 未识别的原始取值，用于展示
    /// 以下为安装器提供的清单元数据；缺失时保持空/-1，绝不复制常量。
    QString version;
    qint64 archiveSize = -1; ///< 字节数；未知为 -1
    QString source;          ///< 官方 release URL
    QString rawLine;
};

/// 解析 `--check-model` 的一行输出。非该事件返回 recognized=false。
ModelCheckEvent ParseInstalledStatusLine(const QString &line);

struct InstallerEvent {
    InstallerEventType type = InstallerEventType::Log;

    QString model;
    QString version;

    /// download 事件：0..100；未知（bytesTotal==0）为 -1。
    int percent = -1;
    qint64 bytesDone = -1;
    qint64 bytesTotal = -1;

    /// extract / verify_file 事件的文件名。
    QString file;

    /// result 事件字段。
    bool ok = false;
    bool published = false;
    bool cancelled = false;
    /// 发布成功但父目录 fsync 失败：持久性未知（断电/崩溃后可能丢失）。
    /// 字段缺失时为 false（兼容旧安装器）。
    bool durabilityUnknown = false;
    /// 发布已发生，但发布后的尽力路径身份检查发现原目标路径不可用/不一致：
    /// 不能声称路径可用，也不可回滚。字段缺失时为 false。
    bool pathStateUncertain = false;
    QString errorCode; ///< "none" / "cancelled" / "archive_mismatch" ...
    QString detail;    ///< 安装器给的中文诊断，仅作参考，界面不依赖其措辞
    QString versionDir;
    qint64 elapsedMs = -1;

    /// 原始行，用于日志视图。
    QString rawLine;
};

/// 解析一行 stdout。永不抛异常。
InstallerEvent ParseInstallerLine(const QString &line);

/// 该事件是否代表安装器已声明最终结果。
bool IsTerminalEvent(const InstallerEvent &event);

} // namespace fcitx::voiceinput

Q_DECLARE_METATYPE(fcitx::voiceinput::InstallerEvent)

#endif // _MODEL_GUI_INSTALLER_PROTOCOL_H_
