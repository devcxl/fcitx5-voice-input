/*
 * SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include "installer_error_text.h"

#include "i18n.h"

namespace fcitx::voiceinput {

QString InstallerErrorText(const QString &code) {
    // 与 src/model_manager/install_events.cpp 的 InstallErrorName 及
    // src/model_manager/README.md §2 的退出码表一一对应。
    if (code == QLatin1String("none") || code.isEmpty()) {
        return {};
    }
    if (code == QLatin1String("usage")) {
        return Tr("安装器参数错误");
    }
    if (code == QLatin1String("unknown_model")) {
        return Tr("安装器不支持该模型标识");
    }
    if (code == QLatin1String("unsafe_path")) {
        return Tr("安装路径不安全");
    }
    if (code == QLatin1String("lock_busy")) {
        return Tr("另一个安装进程正在使用该模型");
    }
    if (code == QLatin1String("cancelled")) {
        return Tr("安装已被取消");
    }
    if (code == QLatin1String("cancelled_unconfirmed")) {
        return Tr("安装已取消，最终状态未确认");
    }
    if (code == QLatin1String("download_failed")) {
        return Tr("下载失败，请检查网络后重试");
    }
    if (code == QLatin1String("archive_mismatch")) {
        return Tr("压缩包校验失败：摘要或大小不符，请重新获取官方压缩包");
    }
    if (code == QLatin1String("archive_unsafe")) {
        return Tr("压缩包包含不允许的条目");
    }
    if (code == QLatin1String("extract_failed")) {
        return Tr("解压失败");
    }
    if (code == QLatin1String("file_mismatch")) {
        return Tr("模型文件校验失败：摘要或大小不符");
    }
    if (code == QLatin1String("missing_file")) {
        return Tr("压缩包缺少必需的模型文件");
    }
    if (code == QLatin1String("trial_load_failed")) {
        return Tr("试加载失败：识别引擎无法加载该模型");
    }
    if (code == QLatin1String("trial_load_timeout")) {
        return Tr("试加载超时");
    }
    if (code == QLatin1String("publish_failed")) {
        return Tr("写入模型目录失败；安装器不会自动覆盖已存在的损坏版本目录，请手动检查后重试");
    }
    if (code == QLatin1String("internal")) {
        return Tr("安装器内部错误");
    }

    // 本模块启动阶段自造的码。
    if (code == QLatin1String("installer_unavailable")) {
        return Tr("模型安装器不可用");
    }
    if (code == QLatin1String("installer_start_failed")) {
        return Tr("无法启动模型安装器");
    }
    if (code == QLatin1String("crashed")) {
        return Tr("安装器进程异常退出");
    }
    if (code == QLatin1String("no_result")) {
        return Tr("安装器未报告安装结果");
    }
    if (code == QLatin1String("empty")) {
        return Tr("构建时未提供安装器路径");
    }
    if (code == QLatin1String("missing")) {
        return Tr("安装器文件不存在");
    }
    if (code == QLatin1String("not_absolute")) {
        return Tr("安装器路径不是绝对路径");
    }
    if (code == QLatin1String("not_executable")) {
        return Tr("安装器没有执行权限");
    }
    if (code == QLatin1String("already_running")) {
        return Tr("已有安装任务正在进行");
    }

    return Tr("安装器返回未知错误（%1）").arg(code);
}

} // namespace fcitx::voiceinput
