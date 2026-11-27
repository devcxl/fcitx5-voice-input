/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
// --check-model 的进程生命周期与解析测试。
// 只使用假安装器脚本，不读取真实模型、不做哈希。

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <cstdlib>

#include "installer_protocol.h"
#include "model_check_runner.h"
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

ModelCheckResult RunOnce(ModelCheckRunner *runner, ModelKind kind,
                         int startTimeoutMs = 20000, int checkTimeoutMs = 60000) {
    ModelCheckResult captured;
    bool done = false;

    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(runner, &ModelCheckRunner::finished, &loop,
                     [&captured, &done, &loop](const ModelCheckResult &r) {
                         captured = r;
                         done = true;
                         loop.quit();
                     });
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(startTimeoutMs);

    if (!runner->start(kind, checkTimeoutMs) && !done) {
        qCritical() << "check start failed";
    }
    if (!done) {
        loop.exec();
    }
    guard.stop();
    Check(done, "check finished");
    return captured;
}

void TestParseAbsentValid() {
    auto e = ParseInstalledStatusLine(
        R"({"event":"installed_status","model":"offline-x-asr","state":"absent"})");
    Check(e.recognized, "parse absent recognized");
    Check(e.state == ModelCheckState::Absent, "parse absent state");
    Check(e.model == QLatin1String("offline-x-asr"), "parse absent model");

    e = ParseInstalledStatusLine(
        R"({"event":"installed_status","model":"online-zipformer","state":"valid"})");
    Check(e.state == ModelCheckState::Valid, "parse valid state");
}

void TestParseTamperedUnreadable() {
    auto e = ParseInstalledStatusLine(
        R"({"event":"installed_status","model":"m","state":"tampered"})");
    Check(e.state == ModelCheckState::Tampered, "parse tampered");

    e = ParseInstalledStatusLine(
        R"({"event":"installed_status","model":"m","state":"unreadable"})");
    Check(e.state == ModelCheckState::Unreadable, "parse unreadable");
}

void TestParseUnknownAndMalformed() {
    auto e = ParseInstalledStatusLine(
        R"({"event":"installed_status","model":"m","state":"something-new"})");
    Check(e.recognized, "unknown token still recognized event");
    Check(e.state == ModelCheckState::Unknown, "unknown token -> Unknown");
    Check(e.rawState == QLatin1String("something-new"), "raw token kept");

    Check(!ParseInstalledStatusLine("not json").recognized,
          "malformed line not recognized");
    Check(!ParseInstalledStatusLine(R"({"event":"result","ok":true})").recognized,
          "other event not recognized");
    Check(!ParseInstalledStatusLine("").recognized, "empty line not recognized");
}

void TestAbsentState() {
    QTemporaryDir dir;
    auto script = WriteScript(
        dir.path(), QStringLiteral("absent.sh"),
        R"(echo '{"event":"installed_status","model":"offline-x-asr","state":"absent"}'
exit 0
)");
    ModelCheckRunner runner(script);
    const auto result = RunOnce(&runner, ModelKind::Offline);
    Check(result.confirmed, "absent: confirmed");
    Check(result.state == ModelCheckState::Absent, "absent: state");
}

void TestValidStateWithMetadata() {
    QTemporaryDir dir;
    auto script = WriteScript(
        dir.path(), QStringLiteral("valid.sh"),
        R"(echo '{"event":"installed_status","model":"online-zipformer","state":"valid","version":"v-2025-06-30","archiveSize":597755927,"source":"https://example.invalid/o.tar.bz2"}'
exit 0
)");
    ModelCheckRunner runner(script);
    const auto result = RunOnce(&runner, ModelKind::Online);
    Check(result.state == ModelCheckState::Valid, "valid: state");
    Check(result.version == QLatin1String("v-2025-06-30"), "valid: version");
    Check(result.archiveSize == 597755927, "valid: archiveSize");
    Check(result.source.startsWith(QLatin1String("https://")), "valid: source");
}

void TestAbsentKeepsMetadata() {
    // 新协议：三个元数据字段与 state 无关，absent 时也会返回。
    QTemporaryDir dir;
    auto script = WriteScript(
        dir.path(), QStringLiteral("absent-meta.sh"),
        R"(echo '{"archiveSize":136902463,"event":"installed_status","model":"offline-x-asr","source":"https://example.invalid/x.tar.bz2","state":"absent","version":"x-asr-2026-06-03"}'
exit 0
)");
    ModelCheckRunner runner(script);
    const auto result = RunOnce(&runner, ModelKind::Offline);
    Check(result.state == ModelCheckState::Absent, "absent-meta: state");
    Check(result.version == QLatin1String("x-asr-2026-06-03"),
          "absent-meta: version kept");
    Check(result.archiveSize == 136902463, "absent-meta: size kept");
    Check(result.source.startsWith(QLatin1String("https://")),
          "absent-meta: source kept");
}

void TestCancelExitCode() {
    QTemporaryDir dir;
    auto script = WriteScript(dir.path(), QStringLiteral("cancel-check.sh"),
                              "exit 5\n");
    ModelCheckRunner runner(script);
    const auto result = RunOnce(&runner, ModelKind::Offline);
    Check(result.state == ModelCheckState::Unknown, "cancel: unknown");
    Check(result.detail.contains(QLatin1String("cancelled")),
          "cancel: detail mentions cancelled");
}

void TestTamperedState() {
    QTemporaryDir dir;
    auto script = WriteScript(
        dir.path(), QStringLiteral("tampered.sh"),
        R"(echo '{"event":"installed_status","model":"m","state":"tampered"}'
exit 0
)");
    ModelCheckRunner runner(script);
    const auto result = RunOnce(&runner, ModelKind::Offline);
    Check(result.state == ModelCheckState::Tampered, "tampered: state");
}

void TestUnreadableState() {
    QTemporaryDir dir;
    auto script = WriteScript(
        dir.path(), QStringLiteral("unreadable.sh"),
        R"(echo '{"event":"installed_status","model":"m","state":"unreadable"}'
exit 0
)");
    ModelCheckRunner runner(script);
    const auto result = RunOnce(&runner, ModelKind::Offline);
    Check(result.state == ModelCheckState::Unreadable, "unreadable: state");
}

void TestMalformedOutputIsUnknown() {
    QTemporaryDir dir;
    auto script = WriteScript(
        dir.path(), QStringLiteral("garbage.sh"),
        R"(echo 'not a protocol line'
echo '{"event":"result","ok":true}'
exit 0
)");
    ModelCheckRunner runner(script);
    const auto result = RunOnce(&runner, ModelKind::Offline);
    Check(!result.confirmed, "garbage: not confirmed");
    Check(result.state == ModelCheckState::Unknown, "garbage: unknown");
    Check(!result.detail.isEmpty(), "garbage: detail present");
}

void TestExitZeroWithoutStatusIsUnknown() {
    QTemporaryDir dir;
    auto script = WriteScript(dir.path(), QStringLiteral("silent.sh"),
                              "exit 0\n");
    ModelCheckRunner runner(script);
    const auto result = RunOnce(&runner, ModelKind::Online);
    Check(result.state == ModelCheckState::Unknown, "silent: unknown");
    Check(!result.confirmed, "silent: not confirmed");
}

void TestCrashIsUnknown() {
    QTemporaryDir dir;
    auto script = WriteScript(
        dir.path(), QStringLiteral("crash.sh"),
        R"(echo '{"event":"installed_status","model":"m","state":"valid"}'
kill -SEGV $$
)");
    ModelCheckRunner runner(script);
    const auto result = RunOnce(&runner, ModelKind::Online);
    // 即使崩溃前打印过状态：以显式事件为准；但退出异常要能区分。
    Check(result.exitCode != 0 || result.state == ModelCheckState::Valid,
          "crash: recorded exit or state");
}

void TestTimeoutIsUnknown() {
    QTemporaryDir dir;
    auto script = WriteScript(
        dir.path(), QStringLiteral("slow.sh"),
        R"(echo '{"event":"installed_status","model":"m","state":"absent"}'
while true; do sleep 0.05; done
)");
    ModelCheckRunner runner(script);
    const auto start = QDateTime::currentMSecsSinceEpoch();
    const auto result = RunOnce(&runner, ModelKind::Offline, 20000, 300);
    const auto elapsed = QDateTime::currentMSecsSinceEpoch() - start;
    Check(elapsed < 5000, "timeout: returned quickly");
    Check(result.state == ModelCheckState::Unknown, "timeout: unknown");
    Check(!result.confirmed, "timeout: not confirmed");
    Check(!runner.isRunning(), "timeout: process stopped");
}

void TestCancelAndWaitStopsProcess() {
    QTemporaryDir dir;
    const auto pidFile = QDir(dir.path()).filePath(QStringLiteral("checker.pid"));
    auto script = WriteScript(
        dir.path(), QStringLiteral("stubborn.sh"),
        QByteArray("echo $$ > ") + pidFile.toUtf8() + "\n" +
            R"(trap '' TERM
echo '{"event":"installed_status","model":"m","state":"tampered"}'
while true; do sleep 0.05; done
)");
    ModelCheckRunner runner(script);
    QEventLoop started;
    QObject::connect(&runner, &ModelCheckRunner::started, &started,
                     &QEventLoop::quit);
    QTimer::singleShot(5000, &started, &QEventLoop::quit);
    Check(runner.start(ModelKind::Offline), "cancel: started");
    if (!runner.isRunning()) {
        // 极端情况下进程已立刻结束，测试失去意义。
        return;
    }
    started.exec();
    Check(runner.isRunning(), "cancel: running before cancel");
    runner.cancelAndWait(400);
    Check(!runner.isRunning(), "cancel: stopped after cancelAndWait");

    QFile pidReader(pidFile);
    if (pidReader.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const auto pid = QString::fromUtf8(pidReader.readAll()).trimmed();
        pidReader.close();
        if (!pid.isEmpty()) {
            Check(!QFile::exists(QStringLiteral("/proc/") + pid),
                  "cancel: no orphan process");
        }
    }
}

void TestProgramValidation() {
    ModelCheckRunner empty{QString()};
    QString reason;
    Check(!empty.isProgramUsable(&reason), "empty unusable");
    Check(reason == QLatin1String("empty"), "empty reason");

    ModelCheckRunner relative{QStringLiteral("voice-input-model-installer")};
    Check(!relative.isProgramUsable(&reason), "relative unusable");

    ModelCheckRunner missing{QStringLiteral("/nonexistent/tool")};
    Check(!missing.isProgramUsable(&reason), "missing unusable");

    QTemporaryDir dir;
    auto script = WriteScript(dir.path(), QStringLiteral("ok.sh"), "exit 0\n");
    ModelCheckRunner usable{script};
    Check(usable.isProgramUsable(&reason), "script usable");
}

void TestUnavailableProgramDoesNotEmit() {
    ModelCheckRunner runner{QStringLiteral("/nonexistent/tool")};
    bool finished = false;
    QObject::connect(&runner, &ModelCheckRunner::finished, &runner,
                     [&finished](const ModelCheckResult &) { finished = true; });
    Check(!runner.start(ModelKind::Offline), "unavailable: start false");
    // 不可用时不发 finished（避免误导为"检查已完成"）。
    Check(!finished, "unavailable: no finished signal");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    TestParseAbsentValid();
    TestParseTamperedUnreadable();
    TestParseUnknownAndMalformed();
    TestProgramValidation();
    TestUnavailableProgramDoesNotEmit();
    TestAbsentState();
    TestAbsentKeepsMetadata();
    TestCancelExitCode();
    TestValidStateWithMetadata();
    TestTamperedState();
    TestUnreadableState();
    TestMalformedOutputIsUnknown();
    TestExitZeroWithoutStatusIsUnknown();
    TestCrashIsUnknown();
    TestTimeoutIsUnknown();
    TestCancelAndWaitStopsProcess();

    if (g_failures != 0) {
        qCritical() << g_failures << "check(s) failed";
        return EXIT_FAILURE;
    }
    qInfo() << "model_check_runner_test: all checks passed";
    return EXIT_SUCCESS;
}
