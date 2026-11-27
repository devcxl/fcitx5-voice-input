/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#ifndef _MODEL_GUI_STATUS_TEXT_H_
#define _MODEL_GUI_STATUS_TEXT_H_

#include <QString>

#include "installer_protocol.h"
#include "model_check_runner.h"
#include "model_status.h"

namespace fcitx::voiceinput {

/// 安装状态文案（来自安装器 `--check-model`，按内置清单摘要校验）。
QString InstallCheckStateText(ModelCheckState state);

/// 引擎可选字段的安装状态文案（仅在无法运行检查时作为兜底）。
QString InstallStateText(StatusFact installed);

/// 识别状态文案。只有 EngineState::Ready 会返回"已就绪"；
/// failed 且仍带 Model/Version 时明确写"加载失败，继续使用旧模型"。
QString RecognitionStateText(const ModelStatus &status);

/// Detail 机器码 → 中文；未识别的码原样返回（不改写语义）。
QString EngineDetailText(const QString &code);

/// 状态详情行（Model/Version/Detail/不可用原因），多条以；连接。
QString StatusDetailText(const ModelStatus &status, const QString &statusUri);

/// 归档大小的可读文本；未知返回"未知"。
QString ArchiveSizeText(qint64 bytes);

/// 模型元数据（版本/下载大小/来源），全部来自安装器输出，缺失即未知。
/// 返回富文本（来源为可点击链接，已转义）。
QString ModelMetaText(const ModelCheckResult &result);

} // namespace fcitx::voiceinput

#endif // _MODEL_GUI_STATUS_TEXT_H_
