/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#ifndef _MODEL_GUI_INSTALLER_ERROR_TEXT_H_
#define _MODEL_GUI_INSTALLER_ERROR_TEXT_H_

#include <QString>

namespace fcitx::voiceinput {

/// 把安装器稳定错误码翻译成用户可读中文。
///
/// 输入是 src/model_manager 的 result.error / 退出码名，
/// 以及本模块在启动阶段自造的 installer_* 码。
/// 未知码原样附在"未知错误（码）"里，便于诊断，但不猜测语义。
QString InstallerErrorText(const QString &code);

} // namespace fcitx::voiceinput

#endif // _MODEL_GUI_INSTALLER_ERROR_TEXT_H_
