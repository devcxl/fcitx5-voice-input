/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#ifndef _MODEL_GUI_I18N_H_
#define _MODEL_GUI_I18N_H_

#include <QString>

#include <fcitx-utils/i18n.h>

namespace fcitx::voiceinput {

/// gettext 域：默认与 addon 共用 fcitx5-voice-input。
/// addon 的 CMake 会定义 FCITX_GETTEXT_DOMAIN；本模块单独构建时退化为同名字面量。
#ifdef FCITX_GETTEXT_DOMAIN
inline constexpr char kTranslationDomain[] = FCITX_GETTEXT_DOMAIN;
#else
inline constexpr char kTranslationDomain[] = "fcitx5-voice-input";
#endif

/// 用户可见文案统一走 fcitx gettext（不用 Qt .ts）。
///
/// 项目约定「中文优先」：源码里的 msgid 直接写中文，因此 zh_CN 无需额外条目
/// 即可正确显示；其它语言在 po/<lang>.po 中补 msgstr 即可翻译。
/// 调用点必须传字符串字面量，才能被 xgettext 提取：
///   Tr("下载并安装")
inline QString Tr(const char *text) {
    return QString::fromUtf8(translateDomain(kTranslationDomain, text));
}

/// 带上下文的重载，对应 xgettext 的 msgctxt。
inline QString Tr(const char *context, const char *text) {
    return QString::fromUtf8(
        translateDomainCtx(kTranslationDomain, context, text));
}

} // namespace fcitx::voiceinput

#endif // _MODEL_GUI_I18N_H_
