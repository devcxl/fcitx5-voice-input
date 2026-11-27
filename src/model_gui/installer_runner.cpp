/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include "installer_runner.h"

#include <QFileInfo>
#include <QProcess>
#include <QTimer>

namespace fcitx::voiceinput {

namespace {

/// 取消宽限期：先 SIGTERM 让安装器清理暂存，超时再 SIGKILL。
/// 安装器自身会处理 SIGINT/SIGTERM/SIGHUP 并输出 result(cancelled)。
constexpr int kCancelKillDelayMs = 5000;

} // namespace

QString ConfiguredInstallerProgram() {
#ifdef VOICE_INPUT_MODEL_INSTALLER
    return QString::fromUtf8(VOICE_INPUT_MODEL_INSTALLER);
#else
    // 未注入构建定义时保持空串：界面明确提示不可用，绝不回退到 PATH 猜测。
    return {};
#endif
}

QString DescribeExitCode(int exitCode) {
    switch (exitCode) {
    case 0:
        return QStringLiteral("ok");
    case 2:
        return QStringLiteral("usage");
    case 3:
        return QStringLiteral("unknown_model");
    case 4:
        return QStringLiteral("lock_busy");
    case 5:
        return QStringLiteral("cancelled");
    case 6:
        return QStringLiteral("archive_mismatch");
    case 7:
        return QStringLiteral("archive_unsafe");
    case 8:
        return QStringLiteral("file_mismatch");
    case 9:
        return QStringLiteral("missing_file");
    case 10:
        return QStringLiteral("trial_load_failed");
    case 11:
        return QStringLiteral("trial_load_timeout");
    case 12:
        return QStringLiteral("download_failed");
    case 13:
        return QStringLiteral("extract_failed");
    case 14:
        return QStringLiteral("publish_failed");
    case 15:
        return QStringLiteral("unsafe_path");
    case 16:
        return QStringLiteral("internal");
    default:
        return QStringLiteral("unknown(%1)").arg(exitCode);
    }
}

InstallerRunner::InstallerRunner(QString program, QObject *parent)
    : QObject(parent), program_(std::move(program)), process_(new QProcess(this)),
      killTimer_(new QTimer(this)) {
    qRegisterMetaType<InstallerEvent>("fcitx::voiceinput::InstallerEvent");
    qRegisterMetaType<InstallerRunner::Result>(
        "fcitx::voiceinput::InstallerRunner::Result");

    killTimer_->setSingleShot(true);
    connect(killTimer_, &QTimer::timeout, this, [this]() {
        if (process_->state() != QProcess::NotRunning) {
            // 宽限期已过，强杀；此时安装器来不及报告 result，最终状态未知。
            killedByTimeout_ = true;
            process_->kill();
        }
    });

    process_->setProcessChannelMode(QProcess::SeparateChannels);

    connect(process_, &QProcess::readyReadStandardOutput, this,
            &InstallerRunner::handleStdout);
    connect(process_, &QProcess::readyReadStandardError, this,
            &InstallerRunner::handleStderr);
    connect(process_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus status) {
                onProcessFinished(exitCode, static_cast<int>(status));
            });
    connect(process_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                onProcessError(static_cast<int>(error));
            });
}

InstallerRunner::~InstallerRunner() { cancelAndWait(); }

bool InstallerRunner::isProgramUsable(QString *reason) const {
    if (program_.isEmpty()) {
        if (reason) {
            *reason = QStringLiteral("empty");
        }
        return false;
    }
    const QFileInfo info(program_);
    if (!info.isAbsolute()) {
        if (reason) {
            *reason = QStringLiteral("not_absolute");
        }
        return false;
    }
    if (!info.exists()) {
        if (reason) {
            *reason = QStringLiteral("missing");
        }
        return false;
    }
    if (!info.isExecutable()) {
        if (reason) {
            *reason = QStringLiteral("not_executable");
        }
        return false;
    }
    return true;
}

bool InstallerRunner::isRunning() const {
    return running_ && process_->state() != QProcess::NotRunning;
}

bool InstallerRunner::start(ModelKind kind, const QString &archivePath,
                            QString *error) {
    if (isRunning()) {
        if (error) {
            *error = QStringLiteral("already_running");
        }
        return false;
    }

    QString reason;
    if (!isProgramUsable(&reason)) {
        if (error) {
            *error = reason;
        }
        Result result;
        result.errorCode = QStringLiteral("installer_unavailable");
        result.message = reason;
        Q_EMIT finished(result);
        return false;
    }

    resetRunState();
    running_ = true;

    QStringList args{QStringLiteral("--model"), ModelKindInstallerArg(kind)};
    if (!archivePath.isEmpty()) {
        args << QStringLiteral("--archive") << archivePath;
    }

    process_->setProgram(program_);
    process_->setArguments(args);
    process_->start();

    if (!process_->waitForStarted(3000)) {
        running_ = false;
        // 稳定错误码交给界面翻译；QProcess 的英文错误串只作为诊断日志。
        Result result;
        result.errorCode = QStringLiteral("installer_start_failed");
        result.message = process_->errorString();
        Q_EMIT finished(result);
        if (error) {
            *error = result.errorCode;
        }
        return false;
    }

    if (error) {
        error->clear();
    }
    Q_EMIT started();
    return true;
}

void InstallerRunner::cancel() {
    if (!running_ || cancelRequested_) {
        return;
    }
    cancelRequested_ = true;
    stopProcess(kCancelKillDelayMs);
}

void InstallerRunner::cancelAndWait(int graceMs) {
    if (!running_) {
        return;
    }
    shuttingDown_ = true;
    cancelRequested_ = true;
    killTimer_->stop();

    if (process_->state() != QProcess::NotRunning) {
        // 安装器处理 SIGTERM 后会删除暂存并退出，先给它清理的机会。
        process_->terminate();
        if (!process_->waitForFinished(graceMs)) {
            process_->kill();
            process_->waitForFinished(1000);
        }
    }
    if (running_) {
        // 阻塞等待期间 finished() 可能尚未送达事件循环：
        // 主动结案，确保 published 等结果不会丢。
        finalizeResult(process_->exitCode(),
                       static_cast<int>(process_->exitStatus()), false);
    }
    running_ = false;
}

void InstallerRunner::stopProcess(int killAfterMs) {
    /* 先把已到达的输出读完，避免取消后丢失 result 事件。 */
    killTimer_->start(killAfterMs);
    process_->terminate();
}

void InstallerRunner::resetRunState() {
    stdoutBuffer_.clear();
    stderrBuffer_.clear();
    cancelRequested_ = false;
    shuttingDown_ = false;
    killedByTimeout_ = false;
    sawResult_ = false;
    resultOk_ = false;
    resultPublished_ = false;
    resultDurabilityUnknown_ = false;
    resultPathStateUncertain_ = false;
    resultCancelled_ = false;
    resultErrorCode_.clear();
    resultDetail_.clear();
    resultVersionDir_.clear();
    lastResult_ = Result{};
}

void InstallerRunner::handleStdout() {
    stdoutBuffer_ += process_->readAllStandardOutput();
    while (true) {
        const int newline = stdoutBuffer_.indexOf('\n');
        if (newline < 0) {
            break;
        }
        QByteArray line = stdoutBuffer_.left(newline);
        stdoutBuffer_.remove(0, newline + 1);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        consumeLine(line);
    }
    // 防御性上限：安装器不会输出超长行，避免异常输入撑爆内存。
    if (stdoutBuffer_.size() > 64 * 1024) {
        const auto chunk = stdoutBuffer_;
        stdoutBuffer_.clear();
        consumeLine(chunk);
    }
}

void InstallerRunner::handleStderr() {
    stderrBuffer_ += process_->readAllStandardError();
    while (true) {
        const int newline = stderrBuffer_.indexOf('\n');
        if (newline < 0) {
            break;
        }
        QByteArray line = stderrBuffer_.left(newline);
        stderrBuffer_.remove(0, newline + 1);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        const auto text = QString::fromUtf8(line).trimmed();
        if (!text.isEmpty() && !shuttingDown_) {
            Q_EMIT logLine(text);
        }
    }
}

void InstallerRunner::consumeLine(const QByteArray &line) {
    const auto text = QString::fromUtf8(line).trimmed();
    if (text.isEmpty()) {
        return;
    }

    const auto event = ParseInstallerLine(text);
    if (event.type == InstallerEventType::Result) {
        sawResult_ = true;
        resultOk_ = event.ok;
        resultPublished_ = event.published;
        resultDurabilityUnknown_ = event.durabilityUnknown;
        resultPathStateUncertain_ = event.pathStateUncertain;
        resultCancelled_ = event.cancelled;
        resultErrorCode_ = event.errorCode;
        resultDetail_ = event.detail;
        resultVersionDir_ = event.versionDir;
    }

    // 析构/关闭路径下只记录结果，不再向正在销毁的界面发信号。
    if (shuttingDown_) {
        return;
    }

    Q_EMIT this->installerEvent(event);

    if (event.type == InstallerEventType::Log) {
        Q_EMIT logLine(text);
    }
}

void InstallerRunner::finalizeResult(int exitCode, int exitStatus,
                                     bool emitSignal) {
    running_ = false;
    killTimer_->stop();

    // 收尾前先把 QProcess 里尚未读出的数据全部取回：
    // cancelAndWait() 走阻塞 waitForFinished()，期间 readyRead 可能没机会触发，
    // 只靠缓冲区会漏掉最后一条 result。
    stdoutBuffer_ += process_->readAllStandardOutput();
    stderrBuffer_ += process_->readAllStandardError();

    if (!stdoutBuffer_.isEmpty()) {
        const auto rest = stdoutBuffer_;
        stdoutBuffer_.clear();
        consumeLine(rest);
    }
    if (!stderrBuffer_.isEmpty()) {
        const auto rest = QString::fromUtf8(stderrBuffer_).trimmed();
        stderrBuffer_.clear();
        if (!rest.isEmpty() && !shuttingDown_) {
            Q_EMIT logLine(rest);
        }
    }

    Result result;
    result.exitCode = exitCode;
    result.versionDir = resultVersionDir_;

    if (cancelRequested_ && !sawResult_ && killedByTimeout_) {
        // 用户取消且安装器被强杀：不宣称失败，也不宣称成功。
        result.ok = false;
        result.cancelled = true;
        result.outcomeUnknown = true;
        result.errorCode = QStringLiteral("cancelled_unconfirmed");
        result.message =
            QStringLiteral("installer killed before reporting final state");
        lastResult_ = result;
        if (emitSignal) {
            Q_EMIT finished(result);
        }
        return;
    }

    const bool normalExit = exitStatus == static_cast<int>(QProcess::NormalExit);
    if (!normalExit) {
        result.ok = false;
        result.cancelled = cancelRequested_;
        // 取消后崩溃：既不能报成功，也不能断言"已干净取消"。
        result.outcomeUnknown = cancelRequested_;
        result.errorCode = QStringLiteral("crashed");
        result.message = QStringLiteral("installer crashed");
        lastResult_ = result;
        if (emitSignal) {
            Q_EMIT finished(result);
        }
        return;
    }

    if (sawResult_) {
        result.ok = resultOk_;
        result.published = resultPublished_;
        result.durabilityUnknown = resultDurabilityUnknown_;
        result.pathStateUncertain = resultPathStateUncertain_;
        // 安装器已发布成功即便收到过取消也必须按成功处理，不得误报未安装；
        // 只有在失败时，用户取消意图才作为 cancelled 上报。
        result.cancelled = resultCancelled_ || (cancelRequested_ && !resultOk_);
        result.errorCode = resultErrorCode_;
        result.message = resultDetail_;
        // 防御：安装器报 ok 但退出码非 0 时，以失败计（不宣称成功）。
        if (resultOk_ && exitCode != 0) {
            result.ok = false;
            result.errorCode = DescribeExitCode(exitCode);
        }
        lastResult_ = result;
        if (emitSignal) {
            Q_EMIT finished(result);
        }
        return;
    }

    // 没有 result：可能是信号或崩溃，绝不据此认为安装成功。
    result.ok = false;
    result.cancelled = cancelRequested_ || exitCode == 5;
    if (result.cancelled) {
        result.errorCode = QStringLiteral("cancelled");
    } else if (exitCode == 0) {
        // 退出码 0 但没有 result：不宣称成功。
        result.errorCode = QStringLiteral("no_result");
    } else {
        result.errorCode = DescribeExitCode(exitCode);
    }
    result.message = QStringLiteral("installer exited without result event");
    lastResult_ = result;
    if (emitSignal) {
        Q_EMIT finished(result);
    }
}

void InstallerRunner::onProcessFinished(int exitCode, int exitStatus) {
    if (!running_) {
        return;
    }
    finalizeResult(exitCode, exitStatus, !shuttingDown_);
}

void InstallerRunner::onProcessError(int error) {
    if (!running_) {
        return;
    }
    if (error == static_cast<int>(QProcess::FailedToStart)) {
        running_ = false;
        killTimer_->stop();
        Result result;
        result.errorCode = QStringLiteral("installer_start_failed");
        result.message = process_->errorString();
        Q_EMIT finished(result);
    }
    // CrashExit 也会触发 finished()，统一在那里收尾。
}

} // namespace fcitx::voiceinput
