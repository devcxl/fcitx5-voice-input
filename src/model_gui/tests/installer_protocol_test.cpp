/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
// 协议解析测试：覆盖 src/model_manager 实际输出的 JSON Lines 形态。
// 不依赖 Qt GUI，也不依赖真实安装器/网络。

#include <QCoreApplication>
#include <QDebug>

#include <cstdlib>

#include "installer_protocol.h"

using namespace fcitx::voiceinput;

namespace {

int g_failures = 0;

void Check(bool condition, const char *what) {
    if (!condition) {
        qCritical() << "FAILED:" << what;
        ++g_failures;
    }
}

void TestStart() {
    const auto e = ParseInstallerLine(
        R"({"event":"start","model":"offline-x-asr","version":"v1"})");
    Check(e.type == InstallerEventType::Start, "start type");
    Check(e.model == QLatin1String("offline-x-asr"), "start model");
    Check(e.version == QLatin1String("v1"), "start version");
}

void TestDownload() {
    const auto e = ParseInstallerLine(
        R"({"event":"download","model":"offline-x-asr","bytesDone":34225615,"bytesTotal":136902463})");
    Check(e.type == InstallerEventType::Download, "download type");
    Check(e.bytesDone == 34225615, "download bytesDone");
    Check(e.bytesTotal == 136902463, "download bytesTotal");
    // 34225615 / 136902463 ≈ 25.0%
    Check(e.percent >= 24 && e.percent <= 25, "download percent");

    const auto zero = ParseInstallerLine(
        R"({"event":"download","bytesDone":10,"bytesTotal":0})");
    Check(zero.percent == -1, "download unknown total -> -1");
}

void TestStages() {
    struct Case {
        const char *line;
        InstallerEventType type;
    } cases[] = {
        {R"({"event":"verify_archive","model":"m","version":"v"})",
         InstallerEventType::VerifyArchive},
        {R"({"event":"extract","model":"m","file":"tokens.txt"})",
         InstallerEventType::Extract},
        {R"({"event":"verify_file","model":"m","file":"tokens.txt"})",
         InstallerEventType::VerifyFile},
        {R"({"event":"verify_trial","model":"m"})",
         InstallerEventType::VerifyTrial},
        {R"({"event":"publish","model":"m","version":"v"})",
         InstallerEventType::Publish},
    };
    for (const auto &c : cases) {
        const auto e = ParseInstallerLine(QString::fromUtf8(c.line));
        Check(e.type == c.type, c.line);
    }
}

void TestResult() {
    const auto ok = ParseInstallerLine(
        R"({"event":"result","model":"offline-x-asr","version":"v","ok":true,"published":true,"cancelled":false,"error":"none","detail":"安装完成","elapsedMs":13856,"versionDir":"/tmp/models/offline-x-asr/v"})");
    Check(ok.type == InstallerEventType::Result, "result type");
    Check(ok.ok, "result ok");
    Check(ok.published, "result published");
    Check(!ok.cancelled, "result not cancelled");
    Check(ok.errorCode == QLatin1String("none"), "result error none");
    Check(ok.versionDir.endsWith(QLatin1String("/tmp/models/offline-x-asr/v")) ||
              ok.versionDir == QLatin1String("/tmp/models/offline-x-asr/v"),
          "result versionDir");
    Check(ok.elapsedMs == 13856, "result elapsedMs");
    Check(IsTerminalEvent(ok), "result is terminal");

    const auto cancelled = ParseInstallerLine(
        R"({"event":"result","ok":false,"published":false,"cancelled":true,"error":"cancelled","detail":"安装已取消"})");
    Check(cancelled.cancelled, "cancelled flag");
    Check(!cancelled.ok, "cancelled not ok");

    const auto lockBusy = ParseInstallerLine(
        R"({"event":"result","ok":false,"published":false,"cancelled":false,"error":"lock_busy","detail":"另一个安装进程正在使用该模型"})");
    Check(lockBusy.errorCode == QLatin1String("lock_busy"), "error code");
}

void TestInstalledStatusMetadata() {
    const auto e = ParseInstalledStatusLine(
        R"({"archiveSize":136902463,"event":"installed_status","model":"offline-x-asr","source":"https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/x.tar.bz2","state":"absent","version":"sherpa-onnx-x-asr-2026-06-03"})");
    Check(e.recognized, "status recognized");
    Check(e.state == ModelCheckState::Absent, "status absent");
    Check(e.version == QLatin1String("sherpa-onnx-x-asr-2026-06-03"),
          "status version");
    Check(e.archiveSize == 136902463, "status archiveSize");
    Check(e.source.startsWith(QLatin1String("https://")), "status source");

    // 字段缺失必须保持未知，不能补常量。
    const auto missing = ParseInstalledStatusLine(
        R"({"event":"installed_status","model":"m","state":"valid"})");
    Check(missing.version.isEmpty(), "missing version empty");
    Check(missing.archiveSize == -1, "missing archiveSize -1");
    Check(missing.source.isEmpty(), "missing source empty");
}

void TestDurabilityUnknown() {
    const auto e = ParseInstallerLine(
        R"({"event":"result","ok":true,"published":true,"cancelled":false,"durabilityUnknown":true,"error":"none","detail":"目录 fsync 失败，持久性未知"})");
    Check(e.type == InstallerEventType::Result, "durability: result type");
    Check(e.ok, "durability: ok true");
    Check(e.published, "durability: published true");
    Check(e.durabilityUnknown, "durability: flag parsed");
    Check(IsTerminalEvent(e), "durability: terminal");

    // 字段缺失必须按 false 处理（兼容旧安装器）。
    const auto legacy = ParseInstallerLine(
        R"({"event":"result","ok":true,"published":true,"cancelled":false,"error":"none"})");
    Check(!legacy.durabilityUnknown, "durability: missing -> false");
    Check(!legacy.pathStateUncertain, "path: missing -> false");

    const auto path = ParseInstallerLine(
        R"({"event":"result","ok":true,"published":true,"cancelled":false,"pathStateUncertain":true,"error":"none"})");
    Check(path.pathStateUncertain, "path: flag parsed");
    Check(path.ok && path.published, "path: publish facts retained");

    const auto both = ParseInstallerLine(
        R"({"event":"result","ok":true,"published":true,"durabilityUnknown":true,"pathStateUncertain":true,"error":"none"})");
    Check(both.durabilityUnknown && both.pathStateUncertain,
          "both: flags parsed together");
}

void TestMalformed() {
    Check(ParseInstallerLine("not json").type == InstallerEventType::Log,
          "plain text -> log");
    Check(ParseInstallerLine("").type == InstallerEventType::Log,
          "empty -> log");
    Check(ParseInstallerLine("{\"event\":\"future_event\"}").type ==
              InstallerEventType::Log,
          "unknown event -> log");
    Check(ParseInstallerLine("{\"ok\":true}").type == InstallerEventType::Log,
          "no event field -> log");
    Check(ParseInstallerLine("[1,2,3]").type == InstallerEventType::Log,
          "non-object -> log");

    // 未知事件绝不能变成 Result。
    const auto e = ParseInstallerLine(R"({"event":"result_v2","ok":true})");
    Check(e.type != InstallerEventType::Result, "unknown result variant not result");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    TestStart();
    TestDownload();
    TestStages();
    TestResult();
    TestInstalledStatusMetadata();
    TestDurabilityUnknown();
    TestMalformed();

    if (g_failures != 0) {
        qCritical() << g_failures << "check(s) failed";
        return EXIT_FAILURE;
    }
    qInfo() << "installer_protocol_test: all checks passed";
    return EXIT_SUCCESS;
}
