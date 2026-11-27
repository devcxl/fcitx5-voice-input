/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
// 安装结果告警测试：durabilityUnknown 与 pathStateUncertain。
// 二者可同时存在；任一存在时都不得展示为"安装成功/可用"，但仍须通知 addon。

#include <QEventLoop>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>

#include <fcitxqtconfiguiwidget.h>

#include "model_kind.h"
#include "model_manager_widget.h"
#include "support/plugin_test_support.h"

using namespace fcitx::voiceinput;
using plugin_test_support::Check;
using plugin_test_support::WriteScript;

namespace {

struct WarningRun {
    QString phaseText;
    QStringList notified;
};

WarningRun RunWarningCase(const QString &dir, const QString &name,
                          const QByteArray &body) {
    const auto script = WriteScript(dir, name, body);
    ModelManagerWidget widget(ModelKind::Offline, nullptr, script);

    WarningRun out;
    QObject::connect(&widget, &fcitx::FcitxQtConfigUIWidget::saveSubConfig,
                     &widget, [&out](const QString &path) {
                         out.notified.append(path);
                     });

    auto *download =
        widget.findChild<QPushButton *>(QStringLiteral("downloadButton"));
    auto *phase = widget.findChild<QLabel *>(QStringLiteral("phaseLabel"));
    if (!download || !phase) {
        Check(false, "warning: widgets exist");
        return out;
    }

    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    guard.start(10000);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&widget, &fcitx::FcitxQtConfigUIWidget::saveSubConfig,
                     &loop, [&loop](const QString &) { loop.quit(); });
    download->click();
    if (out.notified.isEmpty()) {
        loop.exec();
    }
    guard.stop();
    out.phaseText = phase->text();
    return out;
}

// 持久性未知：显著警告，仍通知 addon；不承诺重装即可修复。
void TestDurabilityUnknownWarning(const QString &dir) {
    const auto run = RunWarningCase(
        dir, QStringLiteral("durability.sh"),
        R"(echo '{"event":"result","ok":true,"published":true,"cancelled":false,"durabilityUnknown":true,"error":"none","detail":"目录 fsync 失败，持久性未知"}'
exit 0
)");
    Check(run.notified.size() == 1, "durability: still notified");
    Check(run.phaseText.contains(QStringLiteral("持久性未知")),
          "durability: warning visible");
    Check(run.phaseText.contains(QStringLiteral("可能丢失")),
          "durability: consequence explained");
    Check(run.phaseText.contains(QStringLiteral("若模型丢失")),
          "durability: actionable guidance");
    Check(!run.phaseText.contains(QStringLiteral("检查磁盘状态后重新安装")),
          "durability: no false repair promise");
    Check(run.phaseText.contains(QStringLiteral("已安装")),
          "durability: installed with warning");
}

// 路径状态不确定：发布事实成立，但不能声称路径可用/成功。
void TestPathStateUncertainWarning(const QString &dir) {
    const auto run = RunWarningCase(
        dir, QStringLiteral("path-uncertain.sh"),
        R"(echo '{"event":"result","ok":true,"published":true,"cancelled":false,"pathStateUncertain":true,"error":"none","detail":"目标路径检查不一致"}'
exit 0
)");
    Check(run.notified.size() == 1, "path: still notified");
    Check(run.phaseText.contains(QStringLiteral("发布已发生")),
          "path: publish fact acknowledged");
    Check(run.phaseText.contains(QStringLiteral("目标路径状态不确定")),
          "path: uncertainty stated");
    Check(run.phaseText.contains(QStringLiteral("刷新状态")),
          "path: asks user to refresh");
    Check(!run.phaseText.contains(QStringLiteral("已安装")),
          "path: does not claim installed");
    Check(!run.phaseText.contains(QStringLiteral("安装成功")),
          "path: does not claim success");
    Check(!run.phaseText.contains(QStringLiteral("已就绪")),
          "path: does not claim readiness");
}

// 两个告警同时存在时必须都可见。
void TestBothWarningsVisible(const QString &dir) {
    const auto run = RunWarningCase(
        dir, QStringLiteral("both-warnings.sh"),
        R"(echo '{"event":"result","ok":true,"published":true,"cancelled":false,"durabilityUnknown":true,"pathStateUncertain":true,"error":"none","detail":"fsync 失败且路径不一致"}'
exit 0
)");
    Check(run.notified.size() == 1, "both: still notified");
    Check(run.phaseText.contains(QStringLiteral("目标路径状态不确定")),
          "both: path warning");
    Check(run.phaseText.contains(QStringLiteral("持久性未知")),
          "both: durability warning");
    Check(!run.phaseText.contains(QStringLiteral("已安装")),
          "both: does not claim installed when path uncertain");
    Check(!run.phaseText.contains(QStringLiteral("安装成功")),
          "both: does not claim success");
    Check(!run.phaseText.contains(QStringLiteral("已就绪")),
          "both: does not claim readiness");
}

// 字段缺失时必须按正常成功处理（兼容旧安装器）。
void TestWarningsMissingIsPlainSuccess(const QString &dir) {
    const auto run = RunWarningCase(
        dir, QStringLiteral("no-warning.sh"),
        R"(echo '{"event":"result","ok":true,"published":true,"cancelled":false,"error":"none","detail":"安装完成"}'
exit 0
)");
    Check(run.notified.size() == 1, "missing: notified");
    Check(run.phaseText.contains(QStringLiteral("安装成功")),
          "missing: plain success");
    Check(!run.phaseText.contains(QStringLiteral("不确定")),
          "missing: no uncertainty");
}

} // namespace

namespace plugin_test_support {

void RunWarningTests(const QString &dir) {
    TestDurabilityUnknownWarning(dir);
    TestPathStateUncertainWarning(dir);
    TestBothWarningsVisible(dir);
    TestWarningsMissingIsPlainSuccess(dir);
}

} // namespace plugin_test_support
