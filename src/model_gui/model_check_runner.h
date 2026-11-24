/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#ifndef _MODEL_GUI_MODEL_CHECK_RUNNER_H_
#define _MODEL_GUI_MODEL_CHECK_RUNNER_H_

#include <QByteArray>
#include <QObject>
#include <QString>

#include "installer_protocol.h"
#include "model_kind.h"

class QProcess;
class QTimer;

namespace fcitx::voiceinput {

/// 安装状态检查结果。
struct ModelCheckResult {
    /// 实际状态。检查失败/超时/无法解析均为 Unknown。
    ModelCheckState state = ModelCheckState::Unknown;
    /// installed_status 里未识别的原始 state（仅诊断）。
    QString rawState;
    /// 安装器输出的 model（若有）。
    QString model;
    /// 清单元数据（来自安装器，缺失时为空/-1）。
    QString version;
    qint64 archiveSize = -1;
    QString source;
    /// 本地诊断信息（英文/原始文本，不用于界面主文案）。
    QString detail;
    int exitCode = -1;
    /// 是否拿到了安装器显式声明的 installed_status。
    bool confirmed = false;
};

/// 异步执行 `voice-input-model-installer --check-model <id>`。
///
/// 设计约束：
/// - 独立 QProcess，绝不与安装进程复用；不阻塞主线程，不在 GUI 内计算哈希；
/// - 只采信安装器输出的 `installed_status` 事件；marker/文件存在性都不算数；
/// - 超时/崩溃/输出异常一律 Unknown（界面显示"不可读/未知"）；
/// - 析构与关窗路径必须确保子进程被回收。
class ModelCheckRunner : public QObject {
    Q_OBJECT
public:
    explicit ModelCheckRunner(QString program, QObject *parent = nullptr);
    ~ModelCheckRunner() override;

    QString program() const { return program_; }
    bool isProgramUsable(QString *reason = nullptr) const;
    bool isRunning() const;

    /// 发起检查。timeoutMs 覆盖安装器按清单逐文件校验的最坏耗时。
    bool start(ModelKind kind, int timeoutMs = 60000);

    /// 取消并立即返回；进程在后台收尾。
    void cancel();

    /// 阻塞取消（析构/关窗用），保证不留孤儿进程。
    void cancelAndWait(int graceMs = 1500);

    /// 最近一次结果（含 cancelAndWait 期间收到的输出）。
    const ModelCheckResult &lastResult() const { return lastResult_; }

Q_SIGNALS:
    void started();
    void finished(const fcitx::voiceinput::ModelCheckResult &result);

private:
    void handleStdout();
    void handleStderr();
    void consumeLine(const QByteArray &line);
    void finalize(int exitCode, int exitStatus, bool emitSignal);
    void resetRunState();

    QString program_;
    QProcess *process_ = nullptr;
    QTimer *timeoutTimer_ = nullptr;
    QByteArray stdoutBuffer_;
    QByteArray stderrBuffer_;
    bool running_ = false;
    bool shuttingDown_ = false;
    bool timedOut_ = false;
    bool sawStatus_ = false;
    ModelCheckResult parsed_;
    ModelCheckResult lastResult_;
    QString stderrTail_;
};

} // namespace fcitx::voiceinput

Q_DECLARE_METATYPE(fcitx::voiceinput::ModelCheckResult)

#endif // _MODEL_GUI_MODEL_CHECK_RUNNER_H_
