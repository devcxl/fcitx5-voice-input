/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#ifndef _MODEL_GUI_MODEL_STATUS_H_
#define _MODEL_GUI_MODEL_STATUS_H_

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

#include "model_kind.h"

namespace fcitx::voiceinput {

/// 三值事实：用于"是否已安装"这类没有中间态的字段。
enum class StatusFact {
    Unknown, ///< 没有可信数据：显示"未知"，绝不猜测
    Yes,
    No,
};

/// 引擎侧运行时状态（GetConfig 返回的 State）。
///
/// 约定取值：`idle` / `loading` / `ready` / `failed`。
/// 只有 `ready` 才能说"已就绪"；`failed` 时若 Model/Version 仍在，
/// 说明重载失败但旧引擎继续服务，界面必须写明"继续使用旧模型"。
enum class EngineState {
    Unknown, ///< 接口不可用或取值无法识别
    Idle,    ///< 引擎未激活/未加载
    Loading, ///< 加载中（Detail 可能为 loading/reloading）
    Ready,   ///< 已就绪
    Failed,  ///< 加载失败
};

/// 引擎通过只读子配置暴露的模型状态。
///
/// 约定 URI：fcitx://config/addon/voiceinput/model/status/{online,offline}
/// 由 addon 的 getSubConfig() 返回一个配置，GetConfig 时以 a{sv} 形式给出
/// 扁平键值（键名大小写不敏感）：
///
///   State   -> idle | loading | ready | failed
///   Detail  -> loading / reloading / init_failed / ...
///   Model   -> 实际运行中引擎的模型标识（重载失败时保留旧值）
///   Version -> 实际运行中引擎的版本（重载失败时保留旧值）
///
/// 兼容更细分的安装字段（存在时才使用）：
///   Installed / InstallState -> true/false 或 installed/not_installed
///
/// 任何字段缺失或取值无法识别都保持 Unknown，不臆测。
struct ModelStatus {
    EngineState state = EngineState::Unknown;
    /// State 原文，用于「未知（引擎返回：…）」展示。
    QString stateRaw;

    /// 可选：引擎若提供了显式安装字段。
    StatusFact installed = StatusFact::Unknown;

    QString detailCode; ///< Detail 原文（机器码或短句）
    QString model;
    QString version;

    /// 查询失败原因（本地化显示，例如"引擎未提供状态接口"）。
    QString unavailableReason;
    /// 原始键值，便于诊断；不参与状态判定。
    QMap<QString, QString> raw;

    /// 是否拿到了可判定的 State，或有安装字段。
    bool hasAnyStatus() const { return state != EngineState::Unknown; }
    /// 引擎是否在继续使用旧模型（failed 且 Model/Version 仍有值）。
    bool usingPreviousModel() const {
        return state == EngineState::Failed &&
               (!model.isEmpty() || !version.isEmpty());
    }
};

/// 通过 Fcitx Controller1.GetConfig 读取只读状态子配置。
///
/// 设计约束：
/// - 异步调用，绝不阻塞界面；
/// - 失败/超时/字段缺失一律映射为 Unknown；
/// - 不把"安装进程退出"或"文件存在"当成 Ready。
class ModelStatusClient : public QObject {
    Q_OBJECT
public:
    explicit ModelStatusClient(QObject *parent = nullptr);

    void query(ModelKind kind);

Q_SIGNALS:
    void statusReady(fcitx::voiceinput::ModelKind kind,
                     const fcitx::voiceinput::ModelStatus &status);

private:
    quint64 generation_ = 0;
};

/// 纯函数：把 GetConfig 返回的 QVariant 解析为 ModelStatus。
/// 供本类与单元测试共用；不依赖 DBus。
ModelStatus ParseStatusPayload(const QVariant &payload);

} // namespace fcitx::voiceinput

#endif // _MODEL_GUI_MODEL_STATUS_H_
