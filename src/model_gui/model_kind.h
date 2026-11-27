/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#ifndef _MODEL_GUI_MODEL_KIND_H_
#define _MODEL_GUI_MODEL_KIND_H_

#include <QString>

namespace fcitx::voiceinput {

/// 本地模型种类。两个配置 URI 对应同一个 Qt ConfigUI 模块的两个 key。
enum class ModelKind { Online, Offline };

/// ConfigUI 工厂传给 create() 的 key，必须与 model_gui.json 的 files 一致。
inline QString ModelKindKey(ModelKind kind) {
    return kind == ModelKind::Online ? QStringLiteral("model/online")
                                     : QStringLiteral("model/offline");
}

/// 安装器 `--model` 参数取值。
inline QString ModelKindInstallerArg(ModelKind kind) {
    return kind == ModelKind::Online ? QStringLiteral("online-zipformer")
                                     : QStringLiteral("offline-x-asr");
}

/// 通知 Fcitx 的配置 URI。saveSubConfig 会把它交给
/// Controller1.SetConfig，最终落到 addon 的 setSubConfig("model/online")。
inline QString ModelKindConfigUri(ModelKind kind) {
    return QStringLiteral("fcitx://config/addon/voiceinput/model/") +
           (kind == ModelKind::Online ? QStringLiteral("online")
                                      : QStringLiteral("offline"));
}

/// 只读模型状态 URI，由引擎侧提供；未实现时 GetConfig 返回错误。
inline QString ModelKindStatusUri(ModelKind kind) {
    return QStringLiteral("fcitx://config/addon/voiceinput/model/status/") +
           (kind == ModelKind::Online ? QStringLiteral("online")
                                      : QStringLiteral("offline"));
}

/// 由 ConfigUI key（如 "model/online"）反解模型种类。
inline bool ModelKindFromKey(const QString &key, ModelKind *out) {
    if (key == QLatin1String("model/online")) {
        if (out) {
            *out = ModelKind::Online;
        }
        return true;
    }
    if (key == QLatin1String("model/offline")) {
        if (out) {
            *out = ModelKind::Offline;
        }
        return true;
    }
    return false;
}

} // namespace fcitx::voiceinput

#endif // _MODEL_GUI_MODEL_KIND_H_
