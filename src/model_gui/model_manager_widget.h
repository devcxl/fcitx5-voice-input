/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#ifndef _MODEL_GUI_MODEL_MANAGER_WIDGET_H_
#define _MODEL_GUI_MODEL_MANAGER_WIDGET_H_

#include <QString>

#include <fcitxqtconfiguiwidget.h>

#include "installer_protocol.h"
#include "installer_runner.h"
#include "model_check_runner.h"
#include "model_kind.h"
#include "model_status.h"

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTimer;
class QShowEvent;
class QHideEvent;

namespace fcitx::voiceinput {

/// 单模型配置页。在线 / 离线共用同一实现，由 ModelKind 决定参数与文案。
class ModelManagerWidget : public fcitx::FcitxQtConfigUIWidget {
    Q_OBJECT
public:
    /// installerProgram 为空表示使用构建期注入的路径；
    /// 非空仅供测试注入替身安装器，生产代码不要传值。
    explicit ModelManagerWidget(ModelKind kind, QWidget *parent = nullptr,
                                const QString &installerProgram = QString());
    ~ModelManagerWidget() override;

    // FcitxQtConfigUIWidget
    void load() override;
    void save() override;
    QString title() override;
    QString icon() override;
    bool asyncSave() override { return false; }

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void buildUi();
    void startInstall(bool fromArchive);
    void onInstallerEvent(const InstallerEvent &event);
    void onInstallFinished(const InstallerRunner::Result &result);
    void setRunningUi(bool running);
    void appendLog(const QString &line);
    void refreshStatus();
    /// 只刷新识别状态（轮询用；不触发安装文件哈希）。
    /// showProgress 仅在首次/手动刷新时显示"查询中…"，避免轮询闪烁。
    void refreshRecognition(bool showProgress);
    /// 异步查询安装状态（独立 QProcess，不在主线程做哈希）。
    void checkInstallState();
    void applyStatus(const ModelStatus &status);
    QString stageText(InstallerEventType type) const;
    /// 把安装器的 error code 翻译成用户可读中文；未知码原样显示。
    QString errorCodeText(const QString &code) const;

    ModelKind kind_;
    InstallerRunner *runner_ = nullptr;
    ModelCheckRunner *checkRunner_ = nullptr;
    ModelStatusClient *statusClient_ = nullptr;

    QLabel *installStatusLabel_ = nullptr;
    QLabel *recognitionStatusLabel_ = nullptr;
    QLabel *detailLabel_ = nullptr;
    QLabel *modelMetaLabel_ = nullptr;
    QLabel *phaseLabel_ = nullptr;
    QLabel *installerPathLabel_ = nullptr;
    QProgressBar *progress_ = nullptr;
    QPlainTextEdit *logView_ = nullptr;
    QPushButton *downloadButton_ = nullptr;
    QPushButton *importButton_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
    QPushButton *refreshButton_ = nullptr;

    bool running_ = false;
    /// 本次安装是否已因「已发布」而通知过，避免关闭时重复通知。
    bool notifiedAfterPublish_ = false;
    ModelStatus lastStatus_;
    QTimer *statusPollTimer_ = nullptr;
    QTimer *statusQueryWatchdog_ = nullptr;
    bool statusQueryPending_ = false;
    ModelCheckResult lastCheck_;
    /// 检查进行中又收到检查请求时，等当前结果后自动重跑一次。
    bool recheckAfterCurrent_ = false;
};

} // namespace fcitx::voiceinput

#endif // _MODEL_GUI_MODEL_MANAGER_WIDGET_H_
