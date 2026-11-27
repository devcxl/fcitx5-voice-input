/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include "installer_protocol.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

namespace fcitx::voiceinput {

namespace {

bool EventTypeFromToken(const QString &token, InstallerEventType *out) {
    // token 与 src/model_manager/install_events.cpp 的 InstallStageName 一致。
    static const struct {
        const char *token;
        InstallerEventType type;
    } kMap[] = {
        {"start", InstallerEventType::Start},
        {"download", InstallerEventType::Download},
        {"verify_archive", InstallerEventType::VerifyArchive},
        {"extract", InstallerEventType::Extract},
        {"verify_file", InstallerEventType::VerifyFile},
        {"verify_trial", InstallerEventType::VerifyTrial},
        {"publish", InstallerEventType::Publish},
        {"result", InstallerEventType::Result},
    };
    for (const auto &entry : kMap) {
        if (token == QLatin1String(entry.token)) {
            *out = entry.type;
            return true;
        }
    }
    return false;
}

QString StringField(const QJsonObject &obj, const char *key) {
    const auto value = obj.value(QLatin1String(key));
    return value.isString() ? value.toString() : QString();
}

qint64 Int64Field(const QJsonObject &obj, const char *key, qint64 fallback = -1) {
    const auto value = obj.value(QLatin1String(key));
    if (!value.isDouble()) {
        return fallback;
    }
    return static_cast<qint64>(value.toDouble());
}

bool BoolField(const QJsonObject &obj, const char *key, bool fallback = false) {
    const auto value = obj.value(QLatin1String(key));
    return value.isBool() ? value.toBool() : fallback;
}

} // namespace

QString ModelCheckStateToken(ModelCheckState state) {
    switch (state) {
    case ModelCheckState::Absent:
        return QStringLiteral("absent");
    case ModelCheckState::Valid:
        return QStringLiteral("valid");
    case ModelCheckState::Tampered:
        return QStringLiteral("tampered");
    case ModelCheckState::Unreadable:
        return QStringLiteral("unreadable");
    case ModelCheckState::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

ModelCheckEvent ParseInstalledStatusLine(const QString &line) {
    ModelCheckEvent event;
    event.rawLine = line;

    const auto trimmed = line.trimmed();
    if (trimmed.isEmpty()) {
        return event;
    }

    QJsonParseError error{};
    const auto doc = QJsonDocument::fromJson(trimmed.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return event;
    }

    const auto obj = doc.object();
    if (StringField(obj, "event") != QLatin1String("installed_status")) {
        return event;
    }

    event.recognized = true;
    event.model = StringField(obj, "model");
    event.rawState = StringField(obj, "state");
    event.version = StringField(obj, "version");
    event.archiveSize = Int64Field(obj, "archiveSize", -1);
    event.source = StringField(obj, "source");

    const auto token = event.rawState.trimmed().toLower();
    if (token == QLatin1String("absent")) {
        event.state = ModelCheckState::Absent;
    } else if (token == QLatin1String("valid")) {
        event.state = ModelCheckState::Valid;
    } else if (token == QLatin1String("tampered")) {
        event.state = ModelCheckState::Tampered;
    } else if (token == QLatin1String("unreadable")) {
        event.state = ModelCheckState::Unreadable;
    } else {
        event.state = ModelCheckState::Unknown;
    }
    return event;
}

InstallerEvent ParseInstallerLine(const QString &line) {
    InstallerEvent event;
    event.rawLine = line;

    const auto trimmed = line.trimmed();
    if (trimmed.isEmpty()) {
        return event;
    }

    QJsonParseError error{};
    const auto doc = QJsonDocument::fromJson(trimmed.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        // 非 JSON 行按日志处理；不推断语义。
        event.type = InstallerEventType::Log;
        event.detail = trimmed;
        return event;
    }

    const auto obj = doc.object();
    const auto token = StringField(obj, "event");
    if (!EventTypeFromToken(token, &event.type)) {
        // 未知/未来新增事件：当作日志显示，不改变界面状态。
        event.type = InstallerEventType::Log;
        event.detail = trimmed;
        return event;
    }

    event.model = StringField(obj, "model");
    event.version = StringField(obj, "version");
    event.file = StringField(obj, "file");

    switch (event.type) {
    case InstallerEventType::Download: {
        event.bytesDone = Int64Field(obj, "bytesDone", 0);
        event.bytesTotal = Int64Field(obj, "bytesTotal", 0);
        if (event.bytesTotal > 0) {
            const double ratio =
                static_cast<double>(event.bytesDone) /
                static_cast<double>(event.bytesTotal);
            event.percent =
                qBound(0, static_cast<int>(ratio * 100.0), 100);
        }
        break;
    }
    case InstallerEventType::Result: {
        event.ok = BoolField(obj, "ok");
        event.published = BoolField(obj, "published");
        event.cancelled = BoolField(obj, "cancelled");
        event.durabilityUnknown = BoolField(obj, "durabilityUnknown");
        event.pathStateUncertain = BoolField(obj, "pathStateUncertain");
        event.errorCode = StringField(obj, "error");
        event.detail = StringField(obj, "detail");
        event.versionDir = StringField(obj, "versionDir");
        event.elapsedMs = Int64Field(obj, "elapsedMs", -1);
        break;
    }
    default:
        break;
    }

    return event;
}

bool IsTerminalEvent(const InstallerEvent &event) {
    return event.type == InstallerEventType::Result;
}

} // namespace fcitx::voiceinput
