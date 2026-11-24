/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
// 插件级测试（Qt 级本地验证）：
// 1. 构建产物是合法的 Qt 插件，IID/元数据与 FcitxQtConfigUIFactory 对齐；
// 2. FcitxQtConfigUIFactory 能识别两个 URI（模拟 addon 目录布局）；
// 3. create() 收到 "model/online" / "model/offline" 并返回对应页面。
// 交互类测试（信号/取消/元数据/持久性警告）见 plugin_widget_checks.cpp。

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QLibrary>
#include <QPluginLoader>
#include <QTemporaryDir>

#include <cstdlib>

#include <fcitxqtconfiguifactory.h>
#include <fcitxqtconfiguiwidget.h>

#include "support/plugin_test_support.h"

using plugin_test_support::Check;
using plugin_test_support::WriteScript;

namespace {

void TestPluginMetadata() {
    const QString pluginFile = QString::fromUtf8(VOICE_INPUT_MODEL_GUI_PLUGIN_FILE);
    Check(QFile::exists(pluginFile), "plugin file exists: " + pluginFile);
    Check(QLibrary::isLibrary(pluginFile), "plugin file is a library");

    QPluginLoader loader(pluginFile);
    const auto meta = loader.metaData();
    Check(meta.value(QStringLiteral("IID")).toString() ==
              QStringLiteral("org.fcitx.Fcitx.FcitxQtConfigUIFactoryInterface"),
          "plugin IID matches FcitxQtConfigUIFactoryInterface");

    const auto inner = meta.value(QStringLiteral("MetaData")).toObject();
    Check(inner.value(QStringLiteral("addon")).toString() ==
              QStringLiteral("voiceinput"),
          "metadata addon == voiceinput");
    const auto files = inner.value(QStringLiteral("files")).toArray();
    QStringList fileList;
    for (const auto &value : files) {
        fileList << value.toString();
    }
    Check(fileList.contains(QStringLiteral("model/online")),
          "metadata files contains model/online");
    Check(fileList.contains(QStringLiteral("model/offline")),
          "metadata files contains model/offline");
}

void TestFactoryRecognition(const QString &fakeInstaller) {
    // 模拟 addon 布局：<dir>/qt6/libvoice-input-model-gui.so
    QTemporaryDir addonRoot;
    QDir root(addonRoot.path());
    Check(root.mkdir(QStringLiteral("qt6")), "create qt6 dir");
    const auto source =
        QString::fromUtf8(VOICE_INPUT_MODEL_GUI_PLUGIN_FILE);
    const auto target = root.filePath(QStringLiteral(
        "qt6/libvoice-input-model-gui.so"));
    Check(QFile::copy(source, target), "copy plugin into fake addon dir");

    // FcitxQtConfigUIFactory 在构造时扫描 addon/qt6，所以必须在构造前设置。
    qputenv("FCITX_ADDON_DIRS", addonRoot.path().toUtf8());

    fcitx::FcitxQtConfigUIFactory factory;

    Check(factory.test(QStringLiteral(
              "fcitx://config/addon/voiceinput/model/online")),
          "factory recognizes model/online URI");
    Check(factory.test(QStringLiteral(
              "fcitx://config/addon/voiceinput/model/offline")),
          "factory recognizes model/offline URI");
    Check(!factory.test(QStringLiteral(
              "fcitx://config/addon/voiceinput/model/unknown")),
          "factory rejects unknown key");

    QScopedPointer<fcitx::FcitxQtConfigUIWidget> online(factory.create(
        QStringLiteral("fcitx://config/addon/voiceinput/model/online")));
    Check(!online.isNull(), "create model/online returns widget");
    if (online) {
        Check(!online->title().isEmpty(), "online title non-empty");
        Check(online->asyncSave() == false, "online asyncSave false");
    }

    QScopedPointer<fcitx::FcitxQtConfigUIWidget> offline(factory.create(
        QStringLiteral("fcitx://config/addon/voiceinput/model/offline")));
    Check(!offline.isNull(), "create model/offline returns widget");
    if (offline) {
        Check(!offline->title().isEmpty(), "offline title non-empty");
        Check(online && offline->title() != online->title(),
              "two URIs yield distinct titles");
    }

    // 说明：fakeInstaller 仅用于下面的 widget 级测试，这里保持接口一致。
    Q_UNUSED(fakeInstaller);
}
} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    TestPluginMetadata();

    QTemporaryDir scripts;
    const auto okScript = WriteScript(
        scripts.path(), QStringLiteral("ok.sh"),
        R"(echo '{"event":"start"}'
echo '{"event":"download","bytesDone":1,"bytesTotal":2}'
echo '{"event":"result","ok":true,"published":true,"cancelled":false,"error":"none","detail":"安装完成","versionDir":"/tmp/model"}'
exit 0
)");

    TestFactoryRecognition(okScript);
    plugin_test_support::RunWidgetInteractionTests(scripts.path());
    plugin_test_support::RunWarningTests(scripts.path());

    if (plugin_test_support::Failures() != 0) {
        qCritical() << plugin_test_support::Failures() << "check(s) failed";
        return EXIT_FAILURE;
    }
    qInfo() << "model_gui_plugin_test: all checks passed";
    return EXIT_SUCCESS;
}
