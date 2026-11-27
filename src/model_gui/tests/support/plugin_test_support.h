/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#ifndef _MODEL_GUI_TESTS_PLUGIN_TEST_SUPPORT_H_
#define _MODEL_GUI_TESTS_PLUGIN_TEST_SUPPORT_H_

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QString>

namespace plugin_test_support {

/// 同一测试可执行文件内共享的失败计数（inline 局部静态跨 TU 唯一）。
inline int &Failures() {
    static int failures = 0;
    return failures;
}

inline void Check(bool condition, const QString &what) {
    if (!condition) {
        qCritical() << "FAILED:" << what;
        ++Failures();
    }
}

inline QString WriteScript(const QString &dir, const QString &name,
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

inline QString WriteScript(const QString &dir, const QByteArray &body) {
    return WriteScript(dir, QStringLiteral("fake.sh"), body);
}

/// 交互类测试（放在 plugin_widget_checks.cpp，避免单文件过长）。
void RunWidgetInteractionTests(const QString &dir);

/// 安装结果告警测试（plugin_warning_checks.cpp）。
void RunWarningTests(const QString &dir);

} // namespace plugin_test_support

#endif // _MODEL_GUI_TESTS_PLUGIN_TEST_SUPPORT_H_
