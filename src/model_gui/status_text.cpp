/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include "status_text.h"

#include <QLocale>
#include <QStringList>

#include "i18n.h"

namespace fcitx::voiceinput {

QString InstallCheckStateText(ModelCheckState state) {
    switch (state) {
    case ModelCheckState::Absent:
        return Tr("未安装");
    case ModelCheckState::Valid:
        return Tr("已安装（已按内置清单校验）");
    case ModelCheckState::Tampered:
        return Tr("已损坏（缺文件、大小或摘要不符，或不是普通文件）——请手动检查模型目录");
    case ModelCheckState::Unreadable:
        return Tr("不可读（存在但无法判定：符号链接或读取失败）——请手动检查模型目录");
    case ModelCheckState::Unknown:
        break;
    }
    return Tr("未知（无法确认安装状态）");
}

QString InstallStateText(StatusFact installed) {
    switch (installed) {
    case StatusFact::Yes:
        return Tr("已安装");
    case StatusFact::No:
        return Tr("未安装");
    case StatusFact::Unknown:
        break;
    }
    // 安装字段是可选扩展；引擎未提供时不能据此推断任何结论。
    return Tr("未知（引擎未返回安装字段）");
}

QString RecognitionStateText(const ModelStatus &status) {
    switch (status.state) {
    case EngineState::Ready:
        return Tr("已就绪");
    case EngineState::Loading:
        return status.detailCode.compare(QLatin1String("reloading"),
                                         Qt::CaseInsensitive) == 0
                   ? Tr("重新加载中…")
                   : Tr("加载中…");
    case EngineState::Idle:
        return Tr("未加载（引擎尚未激活）");
    case EngineState::Failed: {
        // 重载失败但旧引擎仍在服务：必须明确说明，而不是笼统说"不可用"。
        if (status.usingPreviousModel()) {
            QString model = status.model;
            if (model.isEmpty()) {
                model = status.version;
            } else if (!status.version.isEmpty()) {
                model = Tr("%1（%2）").arg(status.model, status.version);
            }
            return Tr("加载失败，继续使用旧模型：%1").arg(model);
        }
        return Tr("加载失败");
    }
    case EngineState::Unknown:
        break;
    }
    if (!status.stateRaw.isEmpty()) {
        // 引擎确实返回了 State，但取值不在约定集合内：显示原文。
        return Tr("未知（引擎返回：%1）").arg(status.stateRaw);
    }
    if (!status.unavailableReason.isEmpty()) {
        return Tr("未知（%1）").arg(status.unavailableReason);
    }
    return Tr("未知（引擎未返回 State）");
}

QString EngineDetailText(const QString &code) {
    if (code.compare(QLatin1String("loading"), Qt::CaseInsensitive) == 0) {
        return Tr("正在加载");
    }
    if (code.compare(QLatin1String("reloading"), Qt::CaseInsensitive) == 0) {
        return Tr("正在重新加载");
    }
    if (code.compare(QLatin1String("init_failed"), Qt::CaseInsensitive) == 0) {
        return Tr("初始化失败");
    }
    return code;
}

QString StatusDetailText(const ModelStatus &status, const QString &statusUri) {
    QStringList details;
    if (!status.model.isEmpty()) {
        details << Tr("模型：%1").arg(status.model);
    }
    if (!status.version.isEmpty()) {
        details << Tr("版本：%1").arg(status.version);
    }
    if (!status.detailCode.isEmpty()) {
        details << Tr("引擎细节：%1").arg(EngineDetailText(status.detailCode));
    }
    if (status.hasAnyStatus() && status.state == EngineState::Unknown) {
        details << Tr("引擎返回的 State 不在约定集合内（%1）。")
                       .arg(status.stateRaw);
    }
    if (!status.hasAnyStatus() && status.installed == StatusFact::Unknown) {
        const auto reason = status.unavailableReason.isEmpty()
                                ? Tr("引擎未提供状态接口")
                                : status.unavailableReason;
        details << Tr("状态接口 %1 暂不可用（%2）；安装成功不等于识别就绪。")
                       .arg(statusUri, reason);
    }
    return details.join(QStringLiteral("；"));
}

QString ArchiveSizeText(qint64 bytes) {
    if (bytes < 0) {
        return Tr("未知");
    }
    const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return Tr("%1 MiB（%2 字节）")
        .arg(QString::number(mib, 'f', 1), QLocale::system().toString(bytes));
}

QString ModelMetaText(const ModelCheckResult &result) {
    QStringList parts;
    parts << Tr("版本：%1").arg(result.version.isEmpty()
                                    ? Tr("未知")
                                    : result.version.toHtmlEscaped());
    parts << Tr("下载大小：%1").arg(ArchiveSizeText(result.archiveSize));
    if (result.source.isEmpty()) {
        parts << Tr("来源：未知");
    } else {
        const auto escaped = result.source.toHtmlEscaped();
        parts << Tr("来源：<a href=\"%1\">%1</a>").arg(escaped);
    }
    return parts.join(QStringLiteral("；"));
}

} // namespace fcitx::voiceinput
