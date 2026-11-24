/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include "model_manager_widget.h"

#include <QCoreApplication>
#include <QHideEvent>
#include <QShowEvent>
#include <QTimer>
#include <QDBusConnection>
#include <QDBusServiceWatcher>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include "i18n.h"
#include "installer_error_text.h"
#include "status_text.h"

namespace fcitx::voiceinput {

namespace {

QString ModelDisplayName(ModelKind kind) {
    return kind == ModelKind::Online ? Tr("在线模型（Sherpa Zipformer 流式）")
                                     : Tr("离线模型（Sherpa X-ASR）");
}

} // namespace

ModelManagerWidget::ModelManagerWidget(ModelKind kind, QWidget *parent,
                                       const QString &installerProgram)
    : FcitxQtConfigUIWidget(parent), kind_(kind),
      runner_(new InstallerRunner(installerProgram.isEmpty()
                                      ? ConfiguredInstallerProgram()
                                      : installerProgram,
                                  this)),
      checkRunner_(new ModelCheckRunner(installerProgram.isEmpty()
                                            ? ConfiguredInstallerProgram()
                                            : installerProgram,
                                        this)),
      statusClient_(new ModelStatusClient(this)) {
    // 有界轮询：只查识别状态（GetConfig），绝不重复做安装文件哈希。
    statusPollTimer_ = new QTimer(this);
    statusPollTimer_->setInterval(2000);
    connect(statusPollTimer_, &QTimer::timeout, this,
            [this]() { refreshRecognition(false); });

    // 防悬挂：D-Bus 超时/丢失时允许下一轮继续，不会永久卡住。
    statusQueryWatchdog_ = new QTimer(this);
    statusQueryWatchdog_->setSingleShot(true);
    statusQueryWatchdog_->setInterval(4500);
    connect(statusQueryWatchdog_, &QTimer::timeout, this,
            [this]() { statusQueryPending_ = false; });

    buildUi();

    connect(runner_, &InstallerRunner::installerEvent, this,
            &ModelManagerWidget::onInstallerEvent);
    connect(runner_, &InstallerRunner::logLine, this,
            &ModelManagerWidget::appendLog);
    connect(runner_, &InstallerRunner::finished, this,
            &ModelManagerWidget::onInstallFinished);

    connect(statusClient_, &ModelStatusClient::statusReady, this,
            [this](ModelKind kind, const ModelStatus &status) {
                if (kind != kind_) {
                    return;
                }
                statusQueryPending_ = false;
                if (statusQueryWatchdog_) {
                    statusQueryWatchdog_->stop();
                }
                applyStatus(status);
            });

    connect(checkRunner_, &ModelCheckRunner::finished, this,
            [this](const ModelCheckResult &result) {
                if (running_) {
                    // 安装进行中：取消检查产生的迟到结果不得覆盖"安装中…"。
                    recheckAfterCurrent_ = true;
                    return;
                }
                lastCheck_ = result;
                installStatusLabel_->setText(
                    InstallCheckStateText(result.state));
                modelMetaLabel_->setText(ModelMetaText(result));
                // 安全复核：安装器保守拒绝覆盖已存在但损坏的版本目录，
                // 这里只提示人工处理，不承诺自动修复或删除。
                if (result.state == ModelCheckState::Tampered ||
                    result.state == ModelCheckState::Unreadable) {
                    const auto hint =
                        Tr("安装器不会自动删除或覆盖已存在的模型目录。"
                           "请手动检查该目录（含隐藏文件与符号链接）后重新安装。");
                    installStatusLabel_->setToolTip(hint);
                    downloadButton_->setToolTip(hint);
                    importButton_->setToolTip(hint);
                } else {
                    installStatusLabel_->setToolTip(QString());
                    downloadButton_->setToolTip(QString());
                    importButton_->setToolTip(QString());
                }
                if (!result.detail.isEmpty()) {
                    appendLog(Tr("安装状态检查：%1").arg(result.detail));
                }
                if (recheckAfterCurrent_) {
                    recheckAfterCurrent_ = false;
                    checkInstallState();
                }
            });

    // fcitx5 重新连接（重启/重登）后重新核对安装状态。
    auto *serviceWatcher = new QDBusServiceWatcher(
        QStringLiteral("org.fcitx.Fcitx5"), QDBusConnection::sessionBus(),
        QDBusServiceWatcher::WatchForRegistration, this);
    connect(serviceWatcher, &QDBusServiceWatcher::serviceRegistered, this,
            [this](const QString &) { refreshStatus(); });

    // 窗口关闭 = 取消安装：QDialog 关闭会触发 aboutToQuit 或析构，
    // 两条路径都保证向安装器发 SIGTERM 并等待其清理暂存。
    if (auto *app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, [this]() {
            if (statusPollTimer_) {
                statusPollTimer_->stop();
            }
            if (statusQueryWatchdog_) {
                statusQueryWatchdog_->stop();
            }
            runner_->cancelAndWait();
            checkRunner_->cancelAndWait();
            // 关闭窗口不一定收到 result。这里读安装器在宽限期内最后一次
            // 显式声明的 published：只要版本目录已原子发布就补发通知，
            // 避免 Fcitx 继续使用旧模型直到重启。
            const auto &last = runner_->lastResult();
            if (last.published && !notifiedAfterPublish_) {
                notifiedAfterPublish_ = true;
                Q_EMIT saveSubConfig(ModelKindConfigUri(kind_));
            }
        });
    }

    load();
}

ModelManagerWidget::~ModelManagerWidget() {
    if (statusPollTimer_) {
        statusPollTimer_->stop();
    }
    if (statusQueryWatchdog_) {
        statusQueryWatchdog_->stop();
    }
    runner_->cancelAndWait();
    checkRunner_->cancelAndWait();
}

void ModelManagerWidget::showEvent(QShowEvent *event) {
    FcitxQtConfigUIWidget::showEvent(event);
    // 页面可见期间自动刷新识别状态（2s，有界、无哈希）。
    if (statusPollTimer_ && !statusPollTimer_->isActive()) {
        statusPollTimer_->start();
    }
    refreshRecognition(false);
}

void ModelManagerWidget::hideEvent(QHideEvent *event) {
    FcitxQtConfigUIWidget::hideEvent(event);
    if (statusPollTimer_) {
        statusPollTimer_->stop();
    }
}

void ModelManagerWidget::load() {
    // 本页没有可持久化编辑的配置；load() 只重置展示并重新查询引擎状态。
    if (!running_) {
        progress_->setRange(0, 100);
        progress_->setValue(0);
        phaseLabel_->setText(Tr("尚未开始安装。"));
    }
    installStatusLabel_->setText(Tr("检查中…"));
    recognitionStatusLabel_->setText(Tr("未知（尚未查询）"));
    detailLabel_->clear();
    refreshStatus();
}

void ModelManagerWidget::save() {
    // 安装成功后的通知通过 saveSubConfig 信号发出，见 onInstallFinished()。
}

QString ModelManagerWidget::title() { return ModelDisplayName(kind_); }

QString ModelManagerWidget::icon() { return QStringLiteral("fcitx"); }

void ModelManagerWidget::startInstall(bool fromArchive) {
    if (running_) {
        return;
    }

    QString archivePath;
    if (fromArchive) {
        archivePath = QFileDialog::getOpenFileName(
            this, Tr("选择官方模型压缩包"), QString(),
            Tr("官方模型压缩包 (*.tar.bz2 *.tar.gz *.tar.xz *.zip);;所有文件 (*)"));
        if (archivePath.isEmpty()) {
            return;
        }
        if (!QFileInfo::exists(archivePath)) {
            phaseLabel_->setText(Tr("所选压缩包不存在：%1").arg(archivePath));
            return;
        }
    }

    QString error;
    if (!runner_->start(kind_, archivePath, &error)) {
        // start() 在安装器不可用/启动失败时会同步发出 finished，
        // 这里只补充日志；错误码统一翻译，避免把英文错误串直接展示给用户。
        if (!error.isEmpty()) {
            const auto reason = errorCodeText(error);
            phaseLabel_->setText(Tr("无法启动安装器：%1").arg(reason));
            appendLog(Tr("无法启动安装器：%1（%2）").arg(reason, error));
        }
        return;
    }

    recheckAfterCurrent_ = false;
    checkRunner_->cancel();
    installStatusLabel_->setText(Tr("安装中…"));
    logView_->clear();
    progress_->setRange(0, 100);
    progress_->setValue(0);
    notifiedAfterPublish_ = false;
    setRunningUi(true);
    phaseLabel_->setStyleSheet(QString());
    phaseLabel_->setToolTip(QString());
    phaseLabel_->setText(Tr("正在启动安装器…"));
    appendLog(fromArchive ? Tr("安装来源：官方压缩包 %1").arg(archivePath)
                          : Tr("安装来源：在线下载官方模型包"));
}

void ModelManagerWidget::setRunningUi(bool running) {
    running_ = running;
    // 安装器不可用时保持按钮禁用：失败的 start() 不应让按钮变回可用。
    const bool usable = runner_->isProgramUsable();
    downloadButton_->setEnabled(!running && usable);
    importButton_->setEnabled(!running && usable);
    refreshButton_->setEnabled(!running);
    cancelButton_->setEnabled(running);
}

void ModelManagerWidget::onInstallerEvent(const InstallerEvent &event) {
    switch (event.type) {
    case InstallerEventType::Download:
        if (event.percent >= 0) {
            progress_->setRange(0, 100);
            progress_->setValue(event.percent);
        } else {
            progress_->setRange(0, 0);
        }
        break;
    case InstallerEventType::Start:
    case InstallerEventType::VerifyArchive:
    case InstallerEventType::Extract:
    case InstallerEventType::VerifyFile:
    case InstallerEventType::VerifyTrial:
    case InstallerEventType::Publish:
        // 这些阶段没有百分比，用忙碌指示而不是伪造进度。
        progress_->setRange(0, 0);
        break;
    case InstallerEventType::Result:
    case InstallerEventType::Log:
        return; // result 由 onInstallFinished() 统一处理
    }

    auto text = stageText(event.type);
    if (!event.file.isEmpty()) {
        text = Tr("%1（%2）").arg(text, event.file);
    }
    phaseLabel_->setText(text);

    if (!event.file.isEmpty()) {
        appendLog(event.file);
    }
}

void ModelManagerWidget::onInstallFinished(
    const InstallerRunner::Result &result) {
    setRunningUi(false);
    progress_->setRange(0, 100);

    // published 优先于 cancelled：安装器的原子发布是临界点，
    // rename 成功后版本目录已生效，即使随后收到取消/信号也必须通知，
    // 否则 Fcitx 会继续使用旧模型直到重启。
    if (result.published || result.ok) {
        progress_->setValue(100);

        // 两个告警可能同时存在，必须都可见；任一存在时都不能说"安装成功"。
        const bool uncertain = result.pathStateUncertain ||
                               result.durabilityUnknown;
        phaseLabel_->setStyleSheet(uncertain
                                       ? QStringLiteral("color: #b35c00;")
                                       : QString());
        // 路径不确定时受控路径可能已不存在，绝不能声称"已安装"。
        const auto pathWarning =
            Tr("发布已发生，但目标路径状态不确定，请检查模型目录后刷新状态");
        const auto durabilityWarning =
            Tr("持久性未知：断电或异常重启后可能丢失，请检查磁盘；"
               "若模型丢失请重新安装");
        if (result.pathStateUncertain && result.durabilityUnknown) {
            phaseLabel_->setText(
                Tr("⚠ %1；且%2。").arg(pathWarning, durabilityWarning));
        } else if (result.pathStateUncertain) {
            phaseLabel_->setText(Tr("⚠ %1。").arg(pathWarning));
        } else if (result.durabilityUnknown) {
            // 持久性单独存在时发布目录仍在原位，"已安装"可以成立。
            phaseLabel_->setText(
                Tr("⚠ 已安装，但%1。").arg(durabilityWarning));
        } else if (result.cancelled) {
            phaseLabel_->setText(
                Tr("安装已取消，但版本目录已发布；正在通知 Fcitx 重新读取模型配置。"));
        } else {
            phaseLabel_->setText(
                Tr("安装器已报告安装成功，正在通知 Fcitx 重新读取模型配置。"));
        }

        phaseLabel_->setToolTip(result.message);
        if (uncertain) {
            // 不重复安装器的"安装完成"措辞，避免被读成完全成功。
            appendLog(Tr("安装器返回：%1").arg(result.message.isEmpty()
                                                   ? Tr("已发布")
                                                   : result.message));
            if (result.pathStateUncertain) {
                appendLog(Tr("警告：%1。").arg(pathWarning));
                if (!result.message.isEmpty()) {
                    appendLog(Tr("详情：%1").arg(result.message));
                }
                // 不提供"重试安装"：目标已存在，重试通常会 publish_failed。
                const auto retryHint =
                    Tr("目标路径已存在且状态异常，重试安装通常会失败；"
                       "建议先点击“刷新状态”重新检查。");
                downloadButton_->setToolTip(retryHint);
                importButton_->setToolTip(retryHint);
            }
            if (result.durabilityUnknown) {
                appendLog(Tr("警告：安装器报告持久性未知（目录 fsync 失败）。"));
            }
        } else {
            appendLog(result.message.isEmpty() ? Tr("安装成功。") : result.message);
        }
        if (!result.versionDir.isEmpty()) {
            appendLog(Tr("模型目录：%1").arg(result.versionDir));
        }
        // 通知 host：MainWindow::saveSubConfig -> Controller1.SetConfig。
        // 载荷为空 RawConfig；addon 应把该调用当作「重新扫描模型」信号。
        notifiedAfterPublish_ = true;
        Q_EMIT saveSubConfig(ModelKindConfigUri(kind_));
        refreshStatus(); // 内含识别状态查询 + 安装状态检查
        return;
    }

    if (result.cancelled || result.outcomeUnknown) {
        phaseLabel_->setStyleSheet(QString());
        progress_->setValue(0);
        if (result.outcomeUnknown) {
            phaseLabel_->setText(
                Tr("安装已取消，但安装器未在宽限期内退出，最终状态未知，"
                   "请点击“刷新状态”确认。"));
            appendLog(Tr("安装已取消（结果未确认）。"));
        } else {
            phaseLabel_->setText(Tr("安装已取消，未通知 Fcitx 变更。"));
            appendLog(Tr("安装已取消。"));
        }
        checkInstallState();
        return;
    }

    phaseLabel_->setStyleSheet(QString());
    progress_->setValue(0);
    const auto reason = errorCodeText(result.errorCode);
    phaseLabel_->setText(Tr("安装失败：%1").arg(reason));
    phaseLabel_->setToolTip(result.message);
    if (!result.message.isEmpty()) {
        appendLog(Tr("安装失败：%1（%2）").arg(reason, result.message));
    } else {
        appendLog(Tr("安装失败：%1").arg(reason));
    }
    // 失败也可能留下半套文件，重新核对真实状态。
    checkInstallState();
}

void ModelManagerWidget::appendLog(const QString &line) {
    if (!line.isEmpty()) {
        logView_->appendPlainText(line);
    }
}

void ModelManagerWidget::refreshStatus() {
    refreshRecognition(true);
    checkInstallState();
}

void ModelManagerWidget::refreshRecognition(bool showProgress) {
    if (statusQueryPending_) {
        return; // 上一次查询未完成：避免请求堆积
    }
    statusQueryPending_ = true;
    if (statusQueryWatchdog_) {
        statusQueryWatchdog_->start();
    }
    if (showProgress) {
        recognitionStatusLabel_->setText(Tr("查询中…"));
    }
    statusClient_->query(kind_);
}

void ModelManagerWidget::checkInstallState() {
    if (running_) {
        return; // 安装中不并发检查，安装结束会再触发
    }
    if (checkRunner_->isRunning()) {
        // 已有检查在跑：等它出结果后自动重跑，避免拿到安装前的旧状态。
        recheckAfterCurrent_ = true;
        return;
    }
    recheckAfterCurrent_ = false;

    QString reason;
    if (!checkRunner_->isProgramUsable(&reason)) {
        lastCheck_ = ModelCheckResult{};
        installStatusLabel_->setText(Tr("未知（无法运行安装状态检查）"));
        modelMetaLabel_->setText(Tr("未知（无法运行安装状态检查）"));
        return;
    }

    installStatusLabel_->setText(Tr("检查中…"));
    if (!checkRunner_->start(kind_)) {
        installStatusLabel_->setText(Tr("未知（无法运行安装状态检查）"));
        modelMetaLabel_->setText(Tr("未知（无法运行安装状态检查）"));
    }
}

void ModelManagerWidget::applyStatus(const ModelStatus &status) {
    lastStatus_ = status;

    // 安装状态由独立检查器负责（引擎不提供 Installed 字段）。
    // 仅当检查器根本无法运行、且引擎恰好给了安装字段时才兜底展示。
    if (!checkRunner_->isProgramUsable() &&
        status.installed != StatusFact::Unknown) {
        installStatusLabel_->setText(InstallStateText(status.installed));
    }

    recognitionStatusLabel_->setText(RecognitionStateText(status));
    detailLabel_->setText(
        StatusDetailText(status, ModelKindStatusUri(kind_)));
}

QString ModelManagerWidget::stageText(InstallerEventType type) const {
    switch (type) {
    case InstallerEventType::Start:
        return Tr("正在准备安装…");
    case InstallerEventType::Download:
        return Tr("正在下载官方模型包…");
    case InstallerEventType::VerifyArchive:
        return Tr("正在校验压缩包…");
    case InstallerEventType::Extract:
        return Tr("正在解压模型文件…");
    case InstallerEventType::VerifyFile:
        return Tr("正在校验模型文件…");
    case InstallerEventType::VerifyTrial:
        return Tr("正在试加载模型…");
    case InstallerEventType::Publish:
        return Tr("正在写入模型目录…");
    case InstallerEventType::Result:
    case InstallerEventType::Log:
        break;
    }
    return {};
}

QString ModelManagerWidget::errorCodeText(const QString &code) const {
    return InstallerErrorText(code);
}


} // namespace fcitx::voiceinput
