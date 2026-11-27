/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include "model_check_runner.h"

#include <QFileInfo>
#include <QProcess>
#include <QTimer>

namespace fcitx::voiceinput {

namespace {

/// 宽限后强杀：检查是只读操作，被取消时无需长时间等待。
constexpr int kCancelKillDelayMs = 3000;
constexpr int kStderrTailLimit = 4096;

} // namespace

ModelCheckRunner::ModelCheckRunner(QString program, QObject *parent)
    : QObject(parent), program_(std::move(program)),
      process_(new QProcess(this)), timeoutTimer_(new QTimer(this)) {
    qRegisterMetaType<ModelCheckResult>("fcitx::voiceinput::ModelCheckResult");

    timeoutTimer_->setSingleShot(true);
    connect(timeoutTimer_, &QTimer::timeout, this, [this]() {
        if (process_->state() != QProcess::NotRunning) {
            timedOut_ = true;
            process_->kill();
        }
    });

    process_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process_, &QProcess::readyReadStandardOutput, this,
            &ModelCheckRunner::handleStdout);
    connect(process_, &QProcess::readyReadStandardError, this,
            &ModelCheckRunner::handleStderr);
    connect(process_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus status) {
                finalize(exitCode, static_cast<int>(status), !shuttingDown_);
            });
    connect(process_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (!running_) {
                    return;
                }
                if (error == QProcess::FailedToStart) {
                    running_ = false;
                    timeoutTimer_->stop();
                    ModelCheckResult result;
                    result.detail = process_->errorString();
                    result.exitCode = -1;
                    lastResult_ = result;
                    if (!shuttingDown_) {
                        Q_EMIT finished(result);
                    }
                }
            });
}

ModelCheckRunner::~ModelCheckRunner() { cancelAndWait(); }

bool ModelCheckRunner::isProgramUsable(QString *reason) const {
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

bool ModelCheckRunner::isRunning() const {
    return running_ && process_->state() != QProcess::NotRunning;
}

bool ModelCheckRunner::start(ModelKind kind, int timeoutMs) {
    if (isRunning()) {
        return false;
    }
    if (!isProgramUsable()) {
        return false;
    }

    resetRunState();
    running_ = true;

    process_->setProgram(program_);
    process_->setArguments(
        {QStringLiteral("--check-model"), ModelKindInstallerArg(kind)});
    process_->start();

    if (!process_->waitForStarted(3000)) {
        running_ = false;
        ModelCheckResult result;
        result.detail = process_->errorString();
        lastResult_ = result;
        Q_EMIT finished(result);
        return false;
    }

    timeoutTimer_->start(timeoutMs);
    Q_EMIT started();
    return true;
}

void ModelCheckRunner::cancel() {
    if (!running_) {
        return;
    }
    process_->terminate();
    QTimer::singleShot(kCancelKillDelayMs, this, [this]() {
        if (process_->state() != QProcess::NotRunning) {
            process_->kill();
        }
    });
}

void ModelCheckRunner::cancelAndWait(int graceMs) {
    if (!running_ && process_->state() == QProcess::NotRunning) {
        return;
    }
    shuttingDown_ = true;
    timeoutTimer_->stop();

    if (process_->state() != QProcess::NotRunning) {
        process_->terminate();
        if (!process_->waitForFinished(graceMs)) {
            process_->kill();
            process_->waitForFinished(1000);
        }
    }
    if (running_) {
        finalize(process_->exitCode(), static_cast<int>(process_->exitStatus()),
                 false);
    }
    running_ = false;
}

void ModelCheckRunner::resetRunState() {
    stdoutBuffer_.clear();
    stderrBuffer_.clear();
    stderrTail_.clear();
    shuttingDown_ = false;
    timedOut_ = false;
    sawStatus_ = false;
    parsed_ = ModelCheckResult{};
    lastResult_ = ModelCheckResult{};
}

void ModelCheckRunner::handleStdout() {
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
    if (stdoutBuffer_.size() > 64 * 1024) {
        const auto chunk = stdoutBuffer_;
        stdoutBuffer_.clear();
        consumeLine(chunk);
    }
}

void ModelCheckRunner::handleStderr() {
    stderrBuffer_ += process_->readAllStandardError();
    while (true) {
        const int newline = stderrBuffer_.indexOf('\n');
        if (newline < 0) {
            break;
        }
        QByteArray line = stderrBuffer_.left(newline);
        stderrBuffer_.remove(0, newline + 1);
        const auto text = QString::fromUtf8(line).trimmed();
        if (!text.isEmpty()) {
            stderrTail_ += text + QLatin1Char('\n');
            if (stderrTail_.size() > kStderrTailLimit) {
                stderrTail_ = stderrTail_.right(kStderrTailLimit);
            }
        }
    }
}

void ModelCheckRunner::consumeLine(const QByteArray &line) {
    const auto text = QString::fromUtf8(line).trimmed();
    if (text.isEmpty()) {
        return;
    }

    const auto event = ParseInstalledStatusLine(text);
    if (!event.recognized) {
        // stdout 只应承载协议事件；其余内容作为诊断保留，不改变状态。
        stderrTail_ += text + QLatin1Char('\n');
        if (stderrTail_.size() > kStderrTailLimit) {
            stderrTail_ = stderrTail_.right(kStderrTailLimit);
        }
        return;
    }

    sawStatus_ = true;
    parsed_.confirmed = true;
    parsed_.state = event.state;
    parsed_.rawState = event.rawState;
    parsed_.model = event.model;
    parsed_.version = event.version;
    parsed_.archiveSize = event.archiveSize;
    parsed_.source = event.source;
}

void ModelCheckRunner::finalize(int exitCode, int exitStatus, bool emitSignal) {
    running_ = false;
    timeoutTimer_->stop();

    // 阻塞取消期间可能没机会触发 readyRead，这里统一取回。
    stdoutBuffer_ += process_->readAllStandardOutput();
    stderrBuffer_ += process_->readAllStandardError();
    if (!stdoutBuffer_.isEmpty()) {
        const auto rest = stdoutBuffer_;
        stdoutBuffer_.clear();
        consumeLine(rest);
    }
    if (!stderrBuffer_.isEmpty()) {
        stderrTail_ += QString::fromUtf8(stderrBuffer_).trimmed();
        stderrBuffer_.clear();
    }

    ModelCheckResult result;
    result.exitCode = exitCode;

    if (sawStatus_ && !timedOut_) {
        // 只有安装器显式声明的状态才可信；退出码不参与状态判定。
        // 超时强制视为未知：进程被我们杀掉，不能确认它已完成校验。
        result = parsed_;
        result.exitCode = exitCode;
    } else {
        result.state = ModelCheckState::Unknown;
        result.confirmed = false;
        if (timedOut_) {
            result.detail = QStringLiteral("check timed out");
        } else if (exitCode == 5) {
            // 安装器语义：被 SIGINT/SIGTERM 打断时不输出状态行。
            result.detail = QStringLiteral("check cancelled");
        } else if (exitStatus != static_cast<int>(QProcess::NormalExit)) {
            result.detail = QStringLiteral("checker crashed");
        } else {
            result.detail = QStringLiteral("no installed_status event");
        }
        if (!stderrTail_.isEmpty()) {
            result.detail += QStringLiteral(": ") + stderrTail_.trimmed();
        }
    }

    lastResult_ = result;
    if (emitSignal) {
        Q_EMIT finished(result);
    }
}

} // namespace fcitx::voiceinput
