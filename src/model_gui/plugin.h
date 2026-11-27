/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#ifndef _MODEL_GUI_PLUGIN_H_
#define _MODEL_GUI_PLUGIN_H_

#include <fcitxqtconfiguiplugin.h>

#include "model_kind.h"

namespace fcitx::voiceinput {

/// Qt ConfigUI 插件入口。
///
/// 元数据（model_gui.json）声明 addon = "voiceinput"、
/// files = ["model/online", "model/offline"]，因此
/// fcitx://config/addon/voiceinput/model/online 与 .../model/offline
/// 都会落到同一模块，create() 收到 "model/online" / "model/offline"。
class ModelGuiPlugin : public fcitx::FcitxQtConfigUIPlugin {
    Q_OBJECT
public:
    Q_PLUGIN_METADATA(IID FcitxQtConfigUIFactoryInterface_iid FILE
                      "model_gui.json")
    explicit ModelGuiPlugin(QObject *parent = nullptr);

    FcitxQtConfigUIWidget *create(const QString &key) override;
};

} // namespace fcitx::voiceinput

#endif // _MODEL_GUI_PLUGIN_H_
