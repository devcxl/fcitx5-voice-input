/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
// 界面构建单独成文件，避免 model_manager_widget.cpp 过长。
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include "i18n.h"
#include "model_manager_widget.h"
#include "status_text.h"

namespace fcitx::voiceinput {

namespace {

QString ModelDisplayName(ModelKind kind) {
    return kind == ModelKind::Online ? Tr("在线模型（Sherpa Zipformer 流式）")
                                     : Tr("离线模型（Sherpa X-ASR）");
}

QString ModelDescription(ModelKind kind) {
    return kind == ModelKind::Online
               ? Tr("语音边录边识别，全部推理在本机完成。")
               : Tr("整段语音在静音后一次性识别，可在断网环境使用。");
}

} // namespace

void ModelManagerWidget::buildUi() {
    auto *outer = new QVBoxLayout(this);

    auto *header = new QLabel(this);
    header->setTextFormat(Qt::RichText);
    header->setWordWrap(true);
    header->setText(QStringLiteral("<b>%1</b><br/>%2")
                        .arg(ModelDisplayName(kind_), ModelDescription(kind_)));
    outer->addWidget(header);

    // ── 状态（数据来自引擎，缺数据一律显示"未知"）────────────────────────
    auto *statusBox = new QGroupBox(Tr("模型状态"), this);
    auto *statusForm = new QFormLayout(statusBox);

    installStatusLabel_ = new QLabel(statusBox);
    installStatusLabel_->setObjectName(QStringLiteral("installStatusLabel"));
    installStatusLabel_->setWordWrap(true);
    statusForm->addRow(Tr("安装状态："), installStatusLabel_);

    recognitionStatusLabel_ = new QLabel(statusBox);
    recognitionStatusLabel_->setObjectName(
        QStringLiteral("recognitionStatusLabel"));
    recognitionStatusLabel_->setWordWrap(true);
    statusForm->addRow(Tr("识别状态："), recognitionStatusLabel_);

    detailLabel_ = new QLabel(statusBox);
    detailLabel_->setWordWrap(true);
    detailLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusForm->addRow(Tr("详情："), detailLabel_);

    modelMetaLabel_ = new QLabel(statusBox);
    modelMetaLabel_->setObjectName(QStringLiteral("modelMetaLabel"));
    modelMetaLabel_->setWordWrap(true);
    modelMetaLabel_->setTextFormat(Qt::RichText);
    modelMetaLabel_->setOpenExternalLinks(true);
    modelMetaLabel_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    modelMetaLabel_->setText(Tr("未知（尚未检查）"));
    statusForm->addRow(Tr("模型信息："), modelMetaLabel_);

    auto *statusButtons = new QHBoxLayout;
    refreshButton_ = new QPushButton(Tr("刷新状态"), statusBox);
    statusButtons->addWidget(refreshButton_);
    statusButtons->addStretch(1);
    statusForm->addRow(QString(), statusButtons);

    outer->addWidget(statusBox);

    // ── 安装 ────────────────────────────────────────────────────────────
    auto *installBox = new QGroupBox(Tr("安装"), this);
    auto *installLayout = new QVBoxLayout(installBox);

    auto *buttons = new QHBoxLayout;
    downloadButton_ = new QPushButton(Tr("下载并安装"), installBox);
    downloadButton_->setObjectName(QStringLiteral("downloadButton"));
    importButton_ = new QPushButton(Tr("从官方压缩包导入…"), installBox);
    importButton_->setObjectName(QStringLiteral("importButton"));
    cancelButton_ = new QPushButton(Tr("取消"), installBox);
    cancelButton_->setObjectName(QStringLiteral("cancelButton"));
    cancelButton_->setEnabled(false);
    buttons->addWidget(downloadButton_);
    buttons->addWidget(importButton_);
    buttons->addStretch(1);
    buttons->addWidget(cancelButton_);
    installLayout->addLayout(buttons);

    progress_ = new QProgressBar(installBox);
    progress_->setRange(0, 100);
    progress_->setValue(0);
    installLayout->addWidget(progress_);

    phaseLabel_ = new QLabel(installBox);
    phaseLabel_->setObjectName(QStringLiteral("phaseLabel"));
    phaseLabel_->setWordWrap(true);
    phaseLabel_->setText(Tr("尚未开始安装。"));
    installLayout->addWidget(phaseLabel_);

    logView_ = new QPlainTextEdit(installBox);
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(500);
    logView_->setPlaceholderText(Tr("安装器输出将显示在这里。"));
    logView_->setMinimumHeight(120);
    installLayout->addWidget(logView_);

    installerPathLabel_ = new QLabel(installBox);
    installerPathLabel_->setWordWrap(true);
    installerPathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    QString reason;
    if (runner_->isProgramUsable(&reason)) {
        installerPathLabel_->setText(Tr("安装器：%1").arg(runner_->program()));
    } else {
        installerPathLabel_->setText(
            Tr("模型安装器不可用：%1。请重新安装插件或联系打包者。")
                .arg(runner_->program().isEmpty()
                         ? Tr("构建时未提供安装器路径")
                         : runner_->program()));
        downloadButton_->setEnabled(false);
        importButton_->setEnabled(false);
    }
    installLayout->addWidget(installerPathLabel_);

    outer->addWidget(installBox);
    outer->addStretch(1);

    connect(downloadButton_, &QPushButton::clicked, this,
            [this]() { startInstall(false); });
    connect(importButton_, &QPushButton::clicked, this,
            [this]() { startInstall(true); });
    connect(cancelButton_, &QPushButton::clicked, this, [this]() {
        cancelButton_->setEnabled(false);
        phaseLabel_->setText(Tr("正在取消安装，等待安装器清理临时文件…"));
        runner_->cancel();
    });
    connect(refreshButton_, &QPushButton::clicked, this,
            [this]() { refreshStatus(); });
}

} // namespace fcitx::voiceinput
