/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
// InstallerRunner 测试：用临时脚本模拟 voice-input-model-installer，
// 验证 JSON Lines 解析、成功判定、失败判定、取消收尾与路径校验。
// 不启动真实安装器、不联网。

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdlib>

#include "installer_protocol.h"
#include "installer_runner.h"
#include "model_kind.h"

using namespace fcitx::voiceinput;

namespace {

int g_failures = 0;

void Check(bool condition, const char *what) {
    if (!condition) {
        qCritical() << "FAILED:" << what;
        ++g_failures;
    }
}

QString WriteScript(const QString &dir, const QString &name,
                    const QByteArray &body) {
    const auto path = QDir(dir).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qCritical() << "cannot write" << path;
        return {};
    }
    file.write("#!/bin/sh\n");
    file.write(body);
    file.close();
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                        QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                        QFileDevice::ExeGroup | QFileDevice::ReadOther |
                        QFileDevice::ExeOther);
    return path;
}

struct RunOutcome {
    bool finished = false;
    InstallerRunner::Result result;
    QList<InstallerEvent> events;
    QStringList logs;
    QStringList argv;
};

/// 运行一次并等待 finished（带超时），避免测试卡死。
RunOutcome RunOnce(InstallerRunner *runner, ModelKind kind,
                   const QString &archive, int timeoutMs = 10000) {
    RunOutcome outcome;
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);

    QObject::connect(runner, &InstallerRunner::installerEvent, &loop,
                     [&outcome](const InstallerEvent &e) {
                         outcome.events.append(e);
                     });
    QObject::connect(runner, &InstallerRunner::logLine, &loop,
                     [&outcome](const QString &line) {
                         outcome.logs.append(line);
                     });
    QObject::connect(runner, &InstallerRunner::finished, &loop,
                     [&outcome, &loop](const InstallerRunner::Result &r) {
                         outcome.finished = true;
                         outcome.result = r;
                         loop.quit();
                     });
    QObject::connect(&guard, &QTimer::timeout, &loop, [&loop]() {
        qCritical() << "timeout waiting for installer";
        loop.quit();
    });
    guard.start(timeoutMs);

    QString error;
    if (!runner->start(kind, archive, &error) && !outcome.finished) {
        qCritical() << "start failed:" << error;
    }
    if (!outcome.finished) {
        loop.exec();
    }
    guard.stop();
    return outcome;
}

void TestSuccess() {
    QTemporaryDir dir;
    const auto script = WriteScript(
        dir.path(), QStringLiteral("ok.sh"),
        R"(echo '{"event":"start","model":"offline-x-asr"}'
echo '{"event":"download","bytesDone":5,"bytesTotal":10}'
echo '{"event":"verify_archive","model":"offline-x-asr"}'
echo '{"event":"extract","file":"tokens.txt"}'
echo '{"event":"result","ok":true,"published":true,"cancelled":false,"error":"none","detail":"安装完成","versionDir":"/tmp/x"}'
exit 0
)");
    InstallerRunner runner(script);
    const auto outcome = RunOnce(&runner, ModelKind::Offline, {});
    Check(outcome.finished, "success: finished");
    Check(outcome.result.ok, "success: ok");
    Check(!outcome.result.cancelled, "success: not cancelled");
    Check(outcome.result.exitCode == 0, "success: exit 0");
    Check(outcome.result.versionDir == QLatin1String("/tmp/x"),
          "success: versionDir");
    Check(outcome.events.size() >= 4, "success: events parsed");

    bool sawDownload = false;
    for (const auto &e : outcome.events) {
        if (e.type == InstallerEventType::Download) {
            sawDownload = true;
            Check(e.percent == 50, "success: download percent");
        }
    }
    Check(sawDownload, "success: download event seen");
}

void TestResultIdempotentNotPublished() {
    QTemporaryDir dir;
    // 幂等安装：published=false 但仍 ok=true。
    const auto script = WriteScript(
        dir.path(), QStringLiteral("idempotent.sh"),
        R"(echo '{"event":"result","ok":true,"published":false,"cancelled":false,"error":"none","detail":"已安装"}'
exit 0
)");
    InstallerRunner runner(script);
    const auto outcome = RunOnce(&runner, ModelKind::Online, {});
    Check(outcome.result.ok, "idempotent: ok true");
    Check(!outcome.result.published, "idempotent: published false");
}

void TestDurabilityUnknownResult() {
    QTemporaryDir dir;
    const auto script = WriteScript(
        dir.path(), QStringLiteral("durability.sh"),
        R"(echo '{"event":"result","ok":true,"published":true,"cancelled":false,"durabilityUnknown":true,"error":"none","detail":"目录 fsync 失败"}'
exit 0
)");
    InstallerRunner runner(script);
    const auto outcome = RunOnce(&runner, ModelKind::Offline, {});
    Check(outcome.result.ok, "durability: ok");
    Check(outcome.result.published, "durability: published");
    Check(outcome.result.durabilityUnknown, "durability: flag carried");
}

void TestPathStateUncertainResult() {
    QTemporaryDir dir;
    const auto script = WriteScript(
        dir.path(), QStringLiteral("path-uncertain.sh"),
        R"(echo '{"event":"result","ok":true,"published":true,"cancelled":false,"pathStateUncertain":true,"error":"none","detail":"目标路径检查不一致"}'
exit 0
)");
    InstallerRunner runner(script);
    const auto outcome = RunOnce(&runner, ModelKind::Offline, {});
    Check(outcome.result.ok, "path: ok");
    Check(outcome.result.published, "path: published");
    Check(outcome.result.pathStateUncertain, "path: flag carried");
}

void TestFailureResult() {
    QTemporaryDir dir;
    const auto script = WriteScript(
        dir.path(), QStringLiteral("lock.sh"),
        R"(echo '{"event":"result","ok":false,"published":false,"cancelled":false,"error":"lock_busy","detail":"另一个安装进程正在使用该模型"}'
exit 4
)");
    InstallerRunner runner(script);
    const auto outcome = RunOnce(&runner, ModelKind::Offline, {});
    Check(outcome.finished, "failure: finished");
    Check(!outcome.result.ok, "failure: not ok");
    Check(outcome.result.errorCode == QLatin1String("lock_busy"),
          "failure: error code");
    Check(outcome.result.exitCode == 4, "failure: exit 4");
}

void TestExitZeroWithoutResult() {
    QTemporaryDir dir;
    // 退出码 0 但没有 result：不得宣称成功。
    const auto script = WriteScript(
        dir.path(), QStringLiteral("silent.sh"),
        R"(echo '{"event":"start"}'
echo 'some stderr log' >&2
exit 0
)");
    InstallerRunner runner(script);
    const auto outcome = RunOnce(&runner, ModelKind::Online, {});
    Check(outcome.finished, "no-result: finished");
    Check(!outcome.result.ok, "no-result: not ok");
    Check(outcome.result.errorCode == QLatin1String("no_result"),
          "no-result: error code");
    Check(!outcome.logs.isEmpty(), "no-result: stderr forwarded");
}

void TestCrashExit() {
    QTemporaryDir dir;
    const auto script = WriteScript(
        dir.path(), QStringLiteral("crash.sh"),
        R"(echo '{"event":"start"}'
kill -SEGV $$
)");
    InstallerRunner runner(script);
    const auto outcome = RunOnce(&runner, ModelKind::Offline, {});
    Check(outcome.finished, "crash: finished");
    Check(!outcome.result.ok, "crash: not ok");
    Check(!outcome.result.cancelled, "crash: not cancelled");
}

void TestCancelDeliversResult() {
    QTemporaryDir dir;
    // 安装器收到 SIGTERM 后输出 cancelled result 并退出 5。
    const auto script = WriteScript(
        dir.path(), QStringLiteral("cancellable.sh"),
        R"(RESULT='{"event":"result","ok":false,"published":false,"cancelled":true,"error":"cancelled","detail":"安装已取消"}'
trap 'printf "%s\n" "$RESULT"; exit 5' TERM
echo '{"event":"start"}'
echo '{"event":"download","bytesDone":1,"bytesTotal":100}'
while true; do sleep 0.05; done
)");
    InstallerRunner runner(script);

    RunOutcome outcome;
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&runner, &InstallerRunner::finished, &loop,
                     [&outcome, &loop](const InstallerRunner::Result &r) {
                         outcome.finished = true;
                         outcome.result = r;
                         loop.quit();
                     });
    QObject::connect(&guard, &QTimer::timeout, &loop, [&loop]() {
        qCritical() << "cancel: timeout";
        loop.quit();
    });
    guard.start(10000);

    QString error;
    Check(runner.start(ModelKind::Offline, {}, &error), "cancel: start");
    QTimer::singleShot(300, &runner, [&runner]() { runner.cancel(); });
    if (!outcome.finished) {
        loop.exec();
    }
    guard.stop();

    Check(outcome.finished, "cancel: finished");
    Check(outcome.result.cancelled, "cancel: cancelled");
    Check(!outcome.result.ok, "cancel: not ok");
    Check(!runner.isRunning(), "cancel: not running");
}

void TestCancelAndWaitKillsStubbornProcess() {
    QTemporaryDir dir;
    // 忽略 SIGTERM：cancelAndWait 必须在宽限期后 SIGKILL。
    const auto script = WriteScript(
        dir.path(), QStringLiteral("stubborn.sh"),
        R"(trap '' TERM
echo '{"event":"start"}'
while true; do sleep 0.05; done
)");
    InstallerRunner runner(script);
    QString error;
    QEventLoop started;
    QObject::connect(&runner, &InstallerRunner::started, &started,
                     &QEventLoop::quit);
    QTimer::singleShot(5000, &started, &QEventLoop::quit);
    Check(runner.start(ModelKind::Online, {}, &error), "stubborn: start");
    if (runner.isRunning()) {
        started.exec();
    }
    Check(runner.isRunning(), "stubborn: running before cancel");
    runner.cancelAndWait(500); // 短宽限，测试用
    Check(!runner.isRunning(), "stubborn: stopped after cancelAndWait");
}

void TestCancelAfterPublishReportsPublished() {
    QTemporaryDir dir;
    // 模拟「原子发布后再收到取消」：安装器报告 published=true、ok=true。
    const auto script = WriteScript(
        dir.path(), QStringLiteral("publish-then-cancel.sh"),
        R"(RESULT='{"event":"result","ok":true,"published":true,"cancelled":true,"error":"none","detail":"已发布"}'
trap 'printf "%s\n" "$RESULT"; exit 0' TERM
echo '{"event":"start"}'
while true; do sleep 0.05; done
)");
    InstallerRunner runner(script);
    QString error;
    QEventLoop started;
    QObject::connect(&runner, &InstallerRunner::started, &started,
                     &QEventLoop::quit);
    QTimer::singleShot(5000, &started, &QEventLoop::quit);
    Check(runner.start(ModelKind::Offline, {}, &error), "publish-cancel: start");
    if (runner.isRunning()) {
        started.exec();
    }
    // 关闭窗口路径：cancelAndWait 期间安装器仍报告 published。
    runner.cancelAndWait(3000);
    Check(!runner.isRunning(), "publish-cancel: stopped");
    Check(runner.lastResult().published,
          "publish-cancel: published preserved in lastResult");
    Check(runner.lastResult().ok, "publish-cancel: ok preserved");
}

void TestProgramValidation() {
    InstallerRunner empty{QString()};
    QString reason;
    Check(!empty.isProgramUsable(&reason), "empty program unusable");
    Check(reason == QLatin1String("empty"), "empty reason");

    InstallerRunner relative{QStringLiteral("voice-input-model-installer")};
    Check(!relative.isProgramUsable(&reason), "relative path unusable");
    Check(reason == QLatin1String("not_absolute"), "relative reason");

    InstallerRunner missing{QStringLiteral("/nonexistent/tool")};
    Check(!missing.isProgramUsable(&reason), "missing unusable");
    Check(reason == QLatin1String("missing"), "missing reason");

    QTemporaryDir dir;
    const auto script = WriteScript(dir.path(), QStringLiteral("ok.sh"),
                                    "exit 0\n");
    InstallerRunner usable{script};
    Check(usable.isProgramUsable(&reason), "script usable");
}

void TestConfiguredPathIsAbsoluteOrEmpty() {
    const auto configured = ConfiguredInstallerProgram();
    Check(configured.isEmpty() || QFileInfo(configured).isAbsolute(),
          "configured path absolute or empty");
}

void TestNoResultWhenProgramUnavailable() {
    // 路径不可用时应立即 finished（不把界面卡在"进行中"）。
    InstallerRunner runner{QStringLiteral("/nonexistent/tool")};
    bool finished = false;
    InstallerRunner::Result captured;
    QObject::connect(&runner, &InstallerRunner::finished, &runner,
                     [&finished, &captured](const InstallerRunner::Result &r) {
                         finished = true;
                         captured = r;
                     });
    QString error;
    const bool started = runner.start(ModelKind::Online, {}, &error);
    Check(!started, "unavailable: start false");
    Check(finished, "unavailable: finished emitted");
    Check(captured.errorCode == QLatin1String("installer_unavailable"),
          "unavailable: error code");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    TestProgramValidation();
    TestConfiguredPathIsAbsoluteOrEmpty();
    TestNoResultWhenProgramUnavailable();
    TestSuccess();
    TestResultIdempotentNotPublished();
    TestDurabilityUnknownResult();
    TestPathStateUncertainResult();
    TestFailureResult();
    TestExitZeroWithoutResult();
    TestCrashExit();
    TestCancelDeliversResult();
    TestCancelAfterPublishReportsPublished();
    TestCancelAndWaitKillsStubbornProcess();

    if (g_failures != 0) {
        qCritical() << g_failures << "check(s) failed";
        return EXIT_FAILURE;
    }
    qInfo() << "installer_runner_test: all checks passed";
    return EXIT_SUCCESS;
}
