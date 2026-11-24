/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
// 状态解析测试：验证「缺数据 = 未知」，绝不把缺失字段当成已安装/可用。

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusVariant>
#include <QDebug>
#include <QStringList>
#include <QVariantMap>

#include <cstdlib>

#include "model_status.h"

using namespace fcitx::voiceinput;

namespace {

int g_failures = 0;

void Check(bool condition, const char *what) {
    if (!condition) {
        qCritical() << "FAILED:" << what;
        ++g_failures;
    }
}

void TestEmptyPayload() {
    const auto status = ParseStatusPayload(QVariant());
    Check(status.installed == StatusFact::Unknown, "empty: installed unknown");
    Check(status.state == EngineState::Unknown, "empty: state unknown");
    Check(!status.hasAnyStatus(), "empty: no status");
}

void TestReadyState() {
    QVariantMap map;
    map.insert(QStringLiteral("State"), QStringLiteral("ready"));
    map.insert(QStringLiteral("Detail"), QStringLiteral("loading"));
    map.insert(QStringLiteral("Model"), QStringLiteral("offline-x-asr"));
    map.insert(QStringLiteral("Version"), QStringLiteral("2026-06-03"));

    const auto status = ParseStatusPayload(map);
    Check(status.state == EngineState::Ready, "State=ready");
    Check(!status.usingPreviousModel(), "ready: not using previous");
    Check(status.model == QLatin1String("offline-x-asr"), "model");
    Check(status.version == QLatin1String("2026-06-03"), "version");
    Check(status.detailCode == QLatin1String("loading"), "detail code");
}

void TestLoadingState() {
    QVariantMap map;
    map.insert(QStringLiteral("State"), QStringLiteral("loading"));
    map.insert(QStringLiteral("Detail"), QStringLiteral("reloading"));
    const auto status = ParseStatusPayload(map);
    Check(status.state == EngineState::Loading, "State=loading");
    Check(status.detailCode == QLatin1String("reloading"), "reloading detail");
}

void TestIdleState() {
    QVariantMap map;
    map.insert(QStringLiteral("State"), QStringLiteral("idle"));
    const auto status = ParseStatusPayload(map);
    Check(status.state == EngineState::Idle, "State=idle");
}

void TestFailedKeepsPreviousModel() {
    // 重载失败但旧引擎继续服务：State=failed 且 Model/Version 保留。
    QVariantMap map;
    map.insert(QStringLiteral("State"), QStringLiteral("failed"));
    map.insert(QStringLiteral("Detail"), QStringLiteral("init_failed"));
    map.insert(QStringLiteral("Model"), QStringLiteral("online-zipformer"));
    map.insert(QStringLiteral("Version"), QStringLiteral("2025-06-30"));

    const auto status = ParseStatusPayload(map);
    Check(status.state == EngineState::Failed, "State=failed");
    Check(status.usingPreviousModel(), "failed keeps previous model");
    Check(status.model == QLatin1String("online-zipformer"), "model retained");
    Check(status.version == QLatin1String("2025-06-30"), "version retained");
}

void TestFailedWithoutModel() {
    QVariantMap map;
    map.insert(QStringLiteral("State"), QStringLiteral("failed"));
    const auto status = ParseStatusPayload(map);
    Check(status.state == EngineState::Failed, "State=failed (no model)");
    Check(!status.usingPreviousModel(), "no previous model");
}

void TestUnknownStateKept() {
    QVariantMap map;
    map.insert(QStringLiteral("State"), QStringLiteral("warming-up"));
    const auto status = ParseStatusPayload(map);
    Check(status.state == EngineState::Unknown, "unknown state");
    Check(status.stateRaw == QLatin1String("warming-up"), "raw state kept");
    Check(!status.hasAnyStatus(), "unknown state -> no status");
}

void TestOptionalInstalledField() {
    QVariantMap map;
    map.insert(QStringLiteral("State"), QStringLiteral("idle"));
    map.insert(QStringLiteral("Installed"), QStringLiteral("true"));
    auto status = ParseStatusPayload(map);
    Check(status.installed == StatusFact::Yes, "Installed=true");
    Check(status.state == EngineState::Idle, "state independent of installed");

    QVariantMap onlyInstalled;
    onlyInstalled.insert(QStringLiteral("InstallState"),
                         QStringLiteral("not_installed"));
    status = ParseStatusPayload(onlyInstalled);
    Check(status.installed == StatusFact::No, "InstallState=not_installed");
    Check(status.state == EngineState::Unknown, "state unknown without State");
    Check(status.unavailableReason.isEmpty(),
          "no reason when installed field present");
}

void TestUnrecognizedInstallValue() {
    QVariantMap map;
    map.insert(QStringLiteral("State"), QStringLiteral("idle"));
    map.insert(QStringLiteral("Installed"), QStringLiteral("maybe"));
    const auto status = ParseStatusPayload(map);
    Check(status.installed == StatusFact::Unknown, "maybe -> unknown");
}

void TestNestedAndCaseInsensitive() {
    QVariantMap inner;
    inner.insert(QStringLiteral("state"), QStringLiteral("ready"));
    QVariantMap map;
    map.insert(QStringLiteral("Status"), inner);
    const auto status = ParseStatusPayload(map);
    Check(status.state == EngineState::Ready, "nested lowercase state");
}

void TestQDBusVariantWrapping() {
    QVariantMap map;
    map.insert(QStringLiteral("State"), QStringLiteral("ready"));
    const auto status = ParseStatusPayload(QVariant::fromValue(QDBusVariant(map)));
    Check(status.state == EngineState::Ready, "QDBusVariant wrapper");
}

void TestUnavailableReason() {
    QVariantMap map;
    map.insert(QStringLiteral("Something"), QStringLiteral("value"));
    const auto status = ParseStatusPayload(map);
    Check(!status.hasAnyStatus(), "unknown fields -> no status");
    Check(!status.unavailableReason.isEmpty(), "reason present");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    TestEmptyPayload();
    TestReadyState();
    TestLoadingState();
    TestIdleState();
    TestFailedKeepsPreviousModel();
    TestFailedWithoutModel();
    TestUnknownStateKept();
    TestOptionalInstalledField();
    TestUnrecognizedInstallValue();
    TestNestedAndCaseInsensitive();
    TestQDBusVariantWrapping();
    TestUnavailableReason();

    if (g_failures != 0) {
        qCritical() << g_failures << "check(s) failed";
        return EXIT_FAILURE;
    }
    qInfo() << "model_status_test: all checks passed";
    return EXIT_SUCCESS;
}
