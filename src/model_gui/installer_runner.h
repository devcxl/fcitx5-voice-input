/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#ifndef _MODEL_GUI_INSTALLER_RUNNER_H_
#define _MODEL_GUI_INSTALLER_RUNNER_H_

#include <QByteArray>
#include <QObject>
#include <QString>

#include "installer_protocol.h"
#include "model_kind.h"

class QProcess;
class QTimer;

namespace fcitx::voiceinput {

/// 运行 voice-input-model-installer（子进程）。
///
/// 只负责启动、逐行解析 stdout JSON Lines、转发 stderr 日志、取消收尾；
/// 不生成任何用户可见文案。程序路径来自构建期注入的绝对路径
/// （VOICE_INPUT_MODEL_INSTALLER），不依赖 PATH。
class InstallerRunner : public QObject {
    Q_OBJECT
public:
    struct Result {
        /// 安装器显式声明 ok=true（result.ok），且进程正常退出。
        bool ok = false;
        /// 版本目录已原子发布（publihsed=true）；幂等安装时为 false。
        bool published = false;
        /// 发布成功但父目录 fsync 失败：持久性未知，不等同于完全成功。
        bool durabilityUnknown = false;
        /// 发布已发生但目标路径状态不确定：不可声称路径可用或可回滚。
        bool pathStateUncertain = false;
        /// 用户取消或安装器报告 cancelled。
        bool cancelled = false;
        /// 版本目录（成功时可能为空，取决于安装器输出）。
        QString versionDir;
        /// 失败类别，取自 result.error（"lock_busy" 等）；无结果为 "no_result"。
        QString errorCode;
        /// 原始诊断（中文），只用于日志/提示，不做界面状态判定的唯一依据。
        QString message;
        int exitCode = -1;
        /// 取消宽限期超时、安装器被 SIGKILL：最终状态未知（不宣称失败/成功）。
        bool outcomeUnknown = false;
    };

    explicit InstallerRunner(QString program, QObject *parent = nullptr);
    ~InstallerRunner() override;

    QString program() const { return program_; }
    /// 安装器路径是否可用（绝对路径 + 存在 + 可执行）。
    bool isProgramUsable(QString *reason = nullptr) const;

    bool isRunning() const;

    /// archivePath 为空表示按清单 URL 下载；非空表示从官方压缩包安装。
    bool start(ModelKind kind, const QString &archivePath, QString *error);

    /// 请求取消：SIGTERM，5 秒宽限后 SIGKILL。
    void cancel();

    /// 阻塞式取消并等待退出，用于析构 / 窗口关闭，保证不留孤儿进程。
    /// 等待期间若安装器仍完成了原子发布，最终 Result 会记录在 lastResult()，
    /// 调用方可在对象仍存活时补发通知。
    void cancelAndWait(int graceMs = 3000);

    /// 最近一次运行的结果（含 cancelAndWait 期间收到的 result）。
    /// 未运行过时 published/ok 均为 false。
    const Result &lastResult() const { return lastResult_; }

Q_SIGNALS:
    void started();
    /// 命名为 installerEvent，避免与 QObject::event() 同名造成重载歧义。
    void installerEvent(const fcitx::voiceinput::InstallerEvent &event);
    void logLine(const QString &line);
    void finished(const fcitx::voiceinput::InstallerRunner::Result &result);

private:
    void handleStdout();
    void handleStderr();
    void consumeLine(const QByteArray &line);
    /// 汇总最终结果；emitSignal=false 时只记录到 lastResult_（自毁路径）。
    void finalizeResult(int exitCode, int exitStatus, bool emitSignal);
    void onProcessFinished(int exitCode, int exitStatus);
    void onProcessError(int error);
    void stopProcess(int killAfterMs);
    void resetRunState();

    QString program_;
    QProcess *process_ = nullptr;
    QTimer *killTimer_ = nullptr;
    QByteArray stdoutBuffer_;
    QByteArray stderrBuffer_;
    bool running_ = false;
    bool cancelRequested_ = false;
    bool shuttingDown_ = false;
    bool killedByTimeout_ = false;
    bool sawResult_ = false;
    bool resultOk_ = false;
    bool resultPublished_ = false;
    bool resultDurabilityUnknown_ = false;
    bool resultPathStateUncertain_ = false;
    bool resultCancelled_ = false;
    QString resultErrorCode_;
    QString resultDetail_;
    QString resultVersionDir_;
    Result lastResult_;
};

/// 构建期注入的安装器绝对路径；未注入时为空串。
QString ConfiguredInstallerProgram();

/// 安装器退出码语义（见 src/model_manager/README.md §2）。
QString DescribeExitCode(int exitCode);

} // namespace fcitx::voiceinput

Q_DECLARE_METATYPE(fcitx::voiceinput::InstallerRunner::Result)

#endif // _MODEL_GUI_INSTALLER_RUNNER_H_
