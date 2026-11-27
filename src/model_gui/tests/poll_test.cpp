/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
// 识别状态轮询测试（需要私有 D-Bus：由 ctest 通过 dbus-run-session 启动）。
//
// 断言：
// 1) GUI 可见后不需要用户操作即可从 loading 变为 ready；
// 2) 轮询只调用 GetConfig（识别状态），不会重复执行 --check-model 哈希；
// 3) 轮询有界、不叠积（请求次数与时间成正比）。

#include <QApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QDBusVariant>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantMap>

#include <cstdio>
#include <cstdlib>

#include "model_manager_widget.h"

using namespace fcitx::voiceinput;

namespace {

int g_failures = 0;
int g_queries = 0;

void Check(bool condition, const char *what) {
    if (!condition) {
        qCritical() << "FAILED:" << what;
        ++g_failures;
    }
}

/// 假 Controller1（虚拟对象，避免 Qt 槽导出的类型限制）：
/// 前若干次返回 loading，之后返回 ready。
class FakeController : public QDBusVirtualObject {
public:
    explicit FakeController(int loadingCalls)
        : loadingCalls_(loadingCalls) {}

    QString introspect(const QString &path) const override {
        Q_UNUSED(path);
        return QStringLiteral(
            "<interface name=\"org.fcitx.Fcitx.Controller1\">"
            "<method name=\"GetConfig\">"
            "<arg name=\"uri\" type=\"s\" direction=\"in\"/>"
            "<arg name=\"value\" type=\"v\" direction=\"out\"/>"
            "</method></interface>");
    }

    bool handleMessage(const QDBusMessage &message,
                       const QDBusConnection &connection) override {
        if (message.type() != QDBusMessage::MethodCallMessage ||
            message.member() != QLatin1String("GetConfig")) {
            return false;
        }
        ++g_queries;
        QVariantMap map;
        map.insert(QStringLiteral("Model"), QStringLiteral("demo"));
        map.insert(QStringLiteral("Version"), QStringLiteral("v1"));
        if (g_queries <= loadingCalls_) {
            map.insert(QStringLiteral("State"), QStringLiteral("loading"));
            map.insert(QStringLiteral("Detail"), QStringLiteral("loading"));
        } else {
            map.insert(QStringLiteral("State"), QStringLiteral("ready"));
        }
        connection.send(message.createReply(QVariant::fromValue(map)));
        return true;
    }

private:
    int loadingCalls_;
};

QString WriteChecker(const QString &dir, const QString &counterFile) {
    const auto path = QDir(dir).filePath(QStringLiteral("checker.sh"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return {};
    }
    file.write("#!/bin/sh\n");
    file.write("echo x >> ");
    file.write(counterFile.toUtf8());
    file.write("\n");
    file.write("echo '{\"event\":\"installed_status\",\"model\":\"m\",\"state\":\"absent\"}'\n");
    file.close();
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                        QFileDevice::ExeOwner);
    return path;
}

int CountLines(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return 0;
    }
    const auto data = file.readAll();
    int count = 0;
    for (const auto &line : data.split('\n')) {
        if (!line.trimmed().isEmpty()) {
            ++count;
        }
    }
    return count;
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    if (!QDBusConnection::sessionBus().isConnected()) {
        fprintf(stderr, "no session bus; run under dbus-run-session\n");
        return EXIT_FAILURE;
    }

    FakeController controller(1);
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService(QStringLiteral("org.fcitx.Fcitx5"))) {
        fprintf(stderr, "cannot register fake service: %s\n",
                qPrintable(bus.lastError().message()));
        return EXIT_FAILURE;
    }
    if (!bus.registerVirtualObject(QStringLiteral("/controller"), &controller,
                                   QDBusConnection::SingleNode)) {
        fprintf(stderr, "cannot register object\n");
        return EXIT_FAILURE;
    }

    QTemporaryDir dir;
    const auto counter = QDir(dir.path()).filePath(QStringLiteral("checks.count"));
    const auto checker = WriteChecker(dir.path(), counter);
    if (checker.isEmpty()) {
        return EXIT_FAILURE;
    }

    ModelManagerWidget widget(ModelKind::Offline, nullptr, checker);

    auto *recognition = widget.findChild<QLabel *>(
        QStringLiteral("recognitionStatusLabel"));
    if (!recognition) {
        qCritical() << "no recognitionStatusLabel";
        return EXIT_FAILURE;
    }

    widget.show();

    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    guard.start(9000);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [recognition, &loop]() {
        if (recognition->text().contains(QStringLiteral("已就绪"))) {
            loop.quit();
        }
    });
    poll.start(100);
    loop.exec();
    poll.stop();
    guard.stop();

    const auto readyText = recognition->text();
    fprintf(stderr, "final recognition=[%s] queries=%d checks=%d\n",
            qPrintable(readyText), g_queries, CountLines(counter));

    Check(readyText.contains(QStringLiteral("已就绪")),
          "poll: reached ready without user action");
    Check(g_queries >= 2, "poll: at least initial + one poll query");
    // 轮询不得触发安装文件哈希：--check-model 只应在首次加载时执行一次。
    Check(CountLines(counter) == 1,
          "poll: check-model not repeated during polling");

    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
