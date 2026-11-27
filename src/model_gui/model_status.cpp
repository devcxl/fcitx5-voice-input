/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include "model_status.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#include <QVariantMap>

namespace fcitx::voiceinput {

namespace {

constexpr char kFcitxService[] = "org.fcitx.Fcitx5";
constexpr char kFcitxPath[] = "/controller";
constexpr char kFcitxInterface[] = "org.fcitx.Fcitx.Controller1";
constexpr int kQueryTimeoutMs = 3000;

/// 解包 QDBusVariant 外壳。
QVariant Unwrap(const QVariant &value) {
    if (value.userType() == qMetaTypeId<QDBusVariant>()) {
        return value.value<QDBusVariant>().variant();
    }
    return value;
}

/// 展开 a{sv} / 嵌套 map 为「路径 -> 文本」。
///
/// 注意：不要手工 beginMap()/operator>> 迭代 a{sv}——对 fcitx 实际发出的
/// 变体 map，手工迭代会读到错位的键值（实测 key 为空、value 变成下一个键）。
/// qdbus_cast<QVariantMap> 走 Qt 官方解组路径，结果正确。
void FlattenVariant(const QVariant &value, const QString &prefix,
                    QMap<QString, QString> *out) {
    const auto inner = Unwrap(value);

    if (inner.userType() == qMetaTypeId<QDBusArgument>()) {
        const auto argument = inner.value<QDBusArgument>();
        const auto signature = argument.currentSignature();
        if (signature == QLatin1String("a{sv}")) {
            const auto map = qdbus_cast<QVariantMap>(inner);
            for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
                const auto path = prefix.isEmpty()
                                      ? it.key()
                                      : prefix + QLatin1Char('/') + it.key();
                FlattenVariant(it.value(), path, out);
            }
            return;
        }
        if (signature == QLatin1String("s")) {
            if (!prefix.isEmpty()) {
                out->insert(prefix, qdbus_cast<QString>(inner));
            }
            return;
        }
        return;
    }

    if (inner.canConvert<QVariantMap>()) {
        const auto map = inner.toMap();
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            const auto path = prefix.isEmpty()
                                  ? it.key()
                                  : prefix + QLatin1Char('/') + it.key();
            FlattenVariant(it.value(), path, out);
        }
        return;
    }

    if (!prefix.isEmpty() && inner.isValid()) {
        out->insert(prefix, inner.toString());
    }
}

/// 大小写不敏感取值：先比完整路径，再比叶子名。
QString LookupValue(const QMap<QString, QString> &raw,
                    const QStringList &candidates) {
    for (const auto &candidate : candidates) {
        for (auto it = raw.constBegin(); it != raw.constEnd(); ++it) {
            if (it.key().compare(candidate, Qt::CaseInsensitive) == 0) {
                return it.value();
            }
        }
    }
    for (const auto &candidate : candidates) {
        for (auto it = raw.constBegin(); it != raw.constEnd(); ++it) {
            const auto leaf = it.key().section(QLatin1Char('/'), -1);
            if (leaf.compare(candidate, Qt::CaseInsensitive) == 0) {
                return it.value();
            }
        }
    }
    return {};
}

EngineState ParseEnginState(const QString &text) {
    const auto value = text.trimmed().toLower();
    if (value.isEmpty()) {
        return EngineState::Unknown;
    }
    if (value == QLatin1String("idle")) {
        return EngineState::Idle;
    }
    if (value == QLatin1String("loading")) {
        return EngineState::Loading;
    }
    if (value == QLatin1String("ready")) {
        return EngineState::Ready;
    }
    if (value == QLatin1String("failed")) {
        return EngineState::Failed;
    }
    // 未识别的 State 保持 Unknown，并保留原文供界面展示。
    return EngineState::Unknown;
}

/// Installed/InstallState 仅作为可选的补充字段。
StatusFact ParseInstallFact(const QString &text) {
    const auto value = text.trimmed().toLower();
    if (value.isEmpty()) {
        return StatusFact::Unknown;
    }
    if (value == QLatin1String("true") || value == QLatin1String("yes") ||
        value == QLatin1String("1") || value == QLatin1String("installed")) {
        return StatusFact::Yes;
    }
    if (value == QLatin1String("false") || value == QLatin1String("no") ||
        value == QLatin1String("0") || value == QLatin1String("not_installed") ||
        value == QLatin1String("missing")) {
        return StatusFact::No;
    }
    return StatusFact::Unknown;
}

} // namespace

ModelStatus ParseStatusPayload(const QVariant &payload) {
    ModelStatus status;
    FlattenVariant(payload, QString(), &status.raw);

    status.stateRaw = LookupValue(status.raw, {QStringLiteral("State")});
    status.state = ParseEnginState(status.stateRaw);

    status.installed = ParseInstallFact(LookupValue(
        status.raw,
        {QStringLiteral("Installed"), QStringLiteral("InstallState")}));

    status.detailCode =
        LookupValue(status.raw,
                    {QStringLiteral("Detail"), QStringLiteral("Message")});
    status.model = LookupValue(
        status.raw, {QStringLiteral("Model"), QStringLiteral("ModelName")});
    status.version = LookupValue(status.raw, {QStringLiteral("Version")});

    if (!status.hasAnyStatus()) {
        status.unavailableReason =
            status.raw.isEmpty() ? QStringLiteral("引擎返回了空状态配置")
                                 : QStringLiteral("状态配置缺少 State 字段");
        // 没有 State 时，若引擎额外给了安装字段，仍可展示安装事实。
        if (status.installed != StatusFact::Unknown) {
            status.unavailableReason.clear();
        }
    }
    return status;
}

ModelStatusClient::ModelStatusClient(QObject *parent) : QObject(parent) {}

void ModelStatusClient::query(ModelKind kind) {
    const quint64 generation = ++generation_;
    const auto uri = ModelKindStatusUri(kind);

    QDBusMessage message = QDBusMessage::createMethodCall(
        QString::fromLatin1(kFcitxService), QString::fromLatin1(kFcitxPath),
        QString::fromLatin1(kFcitxInterface), QStringLiteral("GetConfig"));
    message << uri;

    // 用原始消息而非 QDBusPendingReply<QDBusVariant>：GetConfig 还带一个
    // 自定义类型的配置描述数组，未注册时按 QVariant 读取更稳妥。
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(message, kQueryTimeoutMs),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, kind, generation]() {
                watcher->deleteLater();
                if (generation != generation_) {
                    return; // 已有更新的查询
                }

                ModelStatus status;
                const auto reply = watcher->reply();
                if (reply.type() == QDBusMessage::ErrorMessage) {
                    // addon 未实现该子配置时正好走这里：
                    // "Failed to get addon config." → 界面显示"未知"。
                    status.unavailableReason =
                        QStringLiteral("引擎未提供状态接口");
                    Q_EMIT statusReady(kind, status);
                    return;
                }

                const auto args = reply.arguments();
                if (args.isEmpty()) {
                    status.unavailableReason =
                        QStringLiteral("引擎未返回状态数据");
                    Q_EMIT statusReady(kind, status);
                    return;
                }

                status = ParseStatusPayload(args.first());
                Q_EMIT statusReady(kind, status);
            });
}

} // namespace fcitx::voiceinput
