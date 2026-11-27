/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
// 插件交互测试：saveSubConfig 信号、取消/关窗、安装状态元数据与
// durabilityUnknown 警告。仅用假安装器脚本，不触发真实下载。
// 单独成文件以保持 plugin_test.cpp 不超过 500 行。

#include <QCoreApplication>
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

void TestSaveSubConfigSignal(const QString &fakeInstaller, ModelKind kind,
                             const QString &expectedUri, const char *label) {
    ModelManagerWidget widget(kind, nullptr, fakeInstaller);

    QStringList notified;
    QObject::connect(&widget,
                     &fcitx::FcitxQtConfigUIWidget::saveSubConfig, &widget,
                     [&notified](const QString &path) {
                         notified.append(path);
                     });

    auto *download = widget.findChild<QPushButton *>(
        QStringLiteral("downloadButton"));
    Check(download != nullptr, QStringLiteral("%1: download button exists")
                                   .arg(QLatin1String(label)));
    if (!download) {
        return;
    }
    Check(download->isEnabled(),
          QStringLiteral("%1: download button enabled").arg(QLatin1String(label)));

    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&widget, &fcitx::FcitxQtConfigUIWidget::saveSubConfig,
                     &loop, [&loop](const QString &) { loop.quit(); });
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(10000);

    download->click();
    if (notified.isEmpty()) {
        loop.exec();
    }
    guard.stop();

    Check(notified.size() == 1,
          QStringLiteral("%1: saveSubConfig emitted once (got %2)")
              .arg(QLatin1String(label))
              .arg(notified.size()));
    if (!notified.isEmpty()) {
        Check(notified.first() == expectedUri,
              QStringLiteral("%1: saveSubConfig path == %2 (got %3)")
                  .arg(QLatin1String(label), expectedUri, notified.first()));
    }
}

void TestCancelDoesNotNotify(const QString &dir, ModelKind kind,
                             const char *label) {
    const auto script = WriteScript(
        dir, QStringLiteral("cancellable-%1.sh").arg(QLatin1String(label)),
        R"(RESULT='{"event":"result","ok":false,"published":false,"cancelled":true,"error":"cancelled","detail":"安装已取消"}'
trap 'printf "%s\n" "$RESULT"; exit 5' TERM
echo '{"event":"start"}'
while true; do sleep 0.05; done
)");
    ModelManagerWidget widget(kind, nullptr, script);

    QStringList notified;
    QObject::connect(&widget, &fcitx::FcitxQtConfigUIWidget::saveSubConfig,
                     &widget, [&notified](const QString &path) {
                         notified.append(path);
                     });

    auto *download =
        widget.findChild<QPushButton *>(QStringLiteral("downloadButton"));
    auto *cancel =
        widget.findChild<QPushButton *>(QStringLiteral("cancelButton"));
    if (!download || !cancel) {
        Check(false, "cancel test: buttons exist");
        return;
    }

    QEventLoop loop;
    QTimer poll;
    QTimer guard;
    guard.setSingleShot(true);
    guard.start(10000);
    // 安装结束会重新启用"下载并安装"；用轮询等待该信号，避免固定等待 10 秒。
    QObject::connect(&poll, &QTimer::timeout, &loop, [download, &loop, &guard]() {
        if (download->isEnabled()) {
            guard.stop();
            loop.quit();
        }
    });
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start(50);

    download->click();
    Check(cancel->isEnabled(), "cancel test: cancel enabled while running");
    QTimer::singleShot(300, cancel, &QPushButton::click);
    loop.exec(); // 等安装器完成取消收尾（download 按钮会重新可用）
    poll.stop();
    guard.stop();

    Check(download->isEnabled(), "cancel test: download re-enabled");
    Check(notified.isEmpty(), "cancel test: no saveSubConfig on cancel");
}

void TestModelMetaUi(const QString &dir) {
    const auto script = WriteScript(
        dir, QStringLiteral("meta.sh"),
        R"(echo '{"event":"installed_status","model":"offline-x-asr","state":"valid","version":"x-asr-2026-06-03","archiveSize":136902463,"source":"https://example.invalid/model.tar.bz2"}'
exit 0
)");
    ModelManagerWidget widget(ModelKind::Offline, nullptr, script);

    auto *meta = widget.findChild<QLabel *>(QStringLiteral("modelMetaLabel"));
    if (!meta) {
        Check(false, "meta: modelMetaLabel exists");
        return;
    }

    QEventLoop loop;
    QTimer poll;
    QTimer guard;
    guard.setSingleShot(true);
    guard.start(8000);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&poll, &QTimer::timeout, &loop, [meta, &loop]() {
        if (meta->text().contains(QLatin1String("x-asr-2026-06-03"))) {
            loop.quit();
        }
    });
    poll.start(50);
    loop.exec();
    poll.stop();
    guard.stop();

    const auto text = meta->text();
    Check(text.contains(QLatin1String("x-asr-2026-06-03")), "meta: version");
    Check(text.contains(QLatin1String("MiB")), "meta: archive size");
    Check(text.contains(QLatin1String("example.invalid")), "meta: source link");
}

void TestModelMetaMissing(const QString &dir) {
    const auto script = WriteScript(
        dir, QStringLiteral("meta-missing.sh"),
        R"(echo '{"event":"installed_status","model":"offline-x-asr","state":"absent"}'
exit 0
)");
    ModelManagerWidget widget(ModelKind::Offline, nullptr, script);

    auto *meta = widget.findChild<QLabel *>(QStringLiteral("modelMetaLabel"));
    if (!meta) {
        Check(false, "meta-missing: label exists");
        return;
    }
    QEventLoop loop;
    QTimer poll;
    QTimer guard;
    guard.setSingleShot(true);
    guard.start(8000);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&poll, &QTimer::timeout, &loop, [meta, &loop]() {
        if (meta->text().contains(QStringLiteral("未知"))) {
            loop.quit();
        }
    });
    poll.start(50);
    loop.exec();
    poll.stop();
    guard.stop();
    Check(meta->text().contains(QStringLiteral("未知")), "meta-missing: 未知");
}

void TestPublishedWinsOverCancelled(const QString &dir) {
    const auto script = WriteScript(
        dir, QStringLiteral("published-cancelled.sh"),
        R"(echo '{"event":"start"}'
echo '{"event":"result","ok":true,"published":true,"cancelled":true,"error":"none","detail":"已发布"}'
exit 0
)");
    ModelManagerWidget widget(ModelKind::Online, nullptr, script);

    QStringList notified;
    QObject::connect(&widget, &fcitx::FcitxQtConfigUIWidget::saveSubConfig,
                     &widget, [&notified](const QString &path) {
                         notified.append(path);
                     });

    auto *download =
        widget.findChild<QPushButton *>(QStringLiteral("downloadButton"));
    if (!download) {
        Check(false, "published-cancel: download button exists");
        return;
    }

    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    guard.start(10000);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&widget, &fcitx::FcitxQtConfigUIWidget::saveSubConfig,
                     &loop, [&loop](const QString &) { loop.quit(); });
    download->click();
    if (notified.isEmpty()) {
        loop.exec();
    }
    guard.stop();

    Check(notified.size() == 1,
          QStringLiteral("published-cancel: notified once (got %1)")
              .arg(notified.size()));
}

void TestCloseAfterPublishNotifies(const QString &dir) {
    const auto script = WriteScript(
        dir, QStringLiteral("publish-on-close.sh"),
        R"(RESULT='{"event":"result","ok":true,"published":true,"cancelled":true,"error":"none","detail":"已发布"}'
trap 'printf "%s\n" "$RESULT"; exit 0' TERM
echo '{"event":"start"}'
while true; do sleep 0.05; done
)");
    ModelManagerWidget widget(ModelKind::Offline, nullptr, script);

    QStringList notified;
    QObject::connect(&widget, &fcitx::FcitxQtConfigUIWidget::saveSubConfig,
                     &widget, [&notified](const QString &path) {
                         notified.append(path);
                     });

    auto *download =
        widget.findChild<QPushButton *>(QStringLiteral("downloadButton"));
    if (!download) {
        Check(false, "close-publish: download button exists");
        return;
    }

    QEventLoop started;
    QTimer guard;
    guard.setSingleShot(true);
    guard.start(5000);
    QObject::connect(&guard, &QTimer::timeout, &started, &QEventLoop::quit);
    QObject::connect(&widget, &fcitx::FcitxQtConfigUIWidget::saveSubConfig,
                     &started, [&started](const QString &) { started.quit(); });
    download->click();
    QTimer::singleShot(400, &started, &QEventLoop::quit); // 让安装器先起来
    started.exec();
    guard.stop();

    // 模拟宿主关闭窗口：aboutToQuit 会 cancelAndWait 并补发通知。
    QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit",
                              Qt::DirectConnection);

    Check(notified.size() == 1,
          QStringLiteral("close-publish: notified once (got %1)")
              .arg(notified.size()));
    if (!notified.isEmpty()) {
        Check(notified.first() ==
                  QStringLiteral(
                      "fcitx://config/addon/voiceinput/model/offline"),
              "close-publish: notification uri");
    }
}

void TestDestroyCancelsInstaller(const QString &dir) {
    const auto pidFile = QDir(dir).filePath(QStringLiteral("installer.pid"));
    const auto script = WriteScript(
        dir, QStringLiteral("long-running.sh"),
        QByteArray("echo $$ > ") + pidFile.toUtf8() + "\n" +
            R"(trap 'exit 0' TERM
echo '{"event":"start"}'
while true; do sleep 0.05; done
)");

    {
        ModelManagerWidget widget(ModelKind::Offline, nullptr, script);
        auto *download =
            widget.findChild<QPushButton *>(QStringLiteral("downloadButton"));
        if (!download) {
            Check(false, "destroy: download button exists");
            return;
        }
        QEventLoop started;
        QFile probe(pidFile);
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &started, [&pidFile, &started]() {
            if (QFile::exists(pidFile)) {
                started.quit();
            }
        });
        QTimer::singleShot(5000, &started, &QEventLoop::quit);
        poll.start(20);
        download->click();
        started.exec();
        poll.stop();
        Check(QFile::exists(pidFile), "destroy: installer started");
    } // widget 析构 => cancelAndWait()

    QFile pidReader(pidFile);
    if (!pidReader.open(QIODevice::ReadOnly | QIODevice::Text)) {
        Check(false, "destroy: pid file readable");
        return;
    }
    const auto pid = QString::fromUtf8(pidReader.readAll()).trimmed();
    pidReader.close();
    // 进程应已退出：/proc/<pid> 不存在或为僵尸（由 init 回收）。
    const bool gone = !QFile::exists(QStringLiteral("/proc/") + pid);
    Check(gone, QStringLiteral("destroy: installer pid %1 terminated").arg(pid));
}

} // namespace

namespace plugin_test_support {

void RunWidgetInteractionTests(const QString &dir) {
    const auto okScript = WriteScript(
        dir, QStringLiteral("ok-interactive.sh"),
        R"(echo '{"event":"start"}'
echo '{"event":"download","bytesDone":1,"bytesTotal":2}'
echo '{"event":"result","ok":true,"published":true,"cancelled":false,"error":"none","detail":"安装完成","versionDir":"/tmp/model"}'
exit 0
)");

    TestSaveSubConfigSignal(okScript, ModelKind::Online,
                             QStringLiteral("fcitx://config/addon/voiceinput/model/online"), "online");
    TestSaveSubConfigSignal(okScript, ModelKind::Offline,
                             QStringLiteral("fcitx://config/addon/voiceinput/model/offline"), "offline");
    TestCancelDoesNotNotify(dir, ModelKind::Offline, "offline");
    TestModelMetaUi(dir);
    TestModelMetaMissing(dir);
    TestPublishedWinsOverCancelled(dir);
    TestCloseAfterPublishNotifies(dir);
    TestDestroyCancelsInstaller(dir);
}

} // namespace plugin_test_support
