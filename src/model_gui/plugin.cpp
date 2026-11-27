/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include "plugin.h"

#include <fcitx-utils/i18n.h>

#include "i18n.h"
#include "model_manager_widget.h"

namespace fcitx::voiceinput {

namespace {

void registerTranslationDomain() {
#ifdef VOICE_INPUT_LOCALE_DIR
    fcitx::registerDomain(kTranslationDomain, VOICE_INPUT_LOCALE_DIR);
#else
    // 未注入 locale 目录时仍注册域名，避免查表失败；
    // 安装后的正常构建会通过 CMake 定义 VOICE_INPUT_LOCALE_DIR。
    fcitx::registerDomain(kTranslationDomain, "");
#endif
}

} // namespace

ModelGuiPlugin::ModelGuiPlugin(QObject *parent) : FcitxQtConfigUIPlugin(parent) {
    registerTranslationDomain();
}

FcitxQtConfigUIWidget *ModelGuiPlugin::create(const QString &key) {
    ModelKind kind;
    if (!ModelKindFromKey(key, &kind)) {
        // 未知 key 不猜测：返回空指针，由 host 报错。
        return nullptr;
    }
    return new ModelManagerWidget(kind, nullptr);
}

} // namespace fcitx::voiceinput
