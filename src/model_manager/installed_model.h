#pragma once

// 已安装版本目录的**只读校验**（addon 与安装器共用）。
//
// 唯一权威是编译期内置清单（model_catalog.h）里的逐文件 sha256 与字节数。
// 版本目录内的 install.json **不可信**：只能用于快速否定（文件不存在 ⇒ 肯定
// 没装好），不能用于肯定。
//
// 校验以 O_NOFOLLOW 打开每个文件（见 sha256.cpp），因此符号链接会直接失败，
// 不会把校验导向别处。

#include <functional>
#include <string>

#include "model_catalog.h"

namespace fcitx::model_manager {

enum class InstalledState {
    /// 版本目录不存在。
    Absent = 0,
    /// 目录存在且四个文件逐一匹配清单。
    Valid,
    /// 目录存在但内容与清单不符（缺文件、大小或摘要不符）。
    Tampered,
    /// 目录存在但无法读取（权限、I/O、路径异常）。
    Unreadable,
};

const char* InstalledStateName(InstalledState state);

struct InstalledCheck {
    InstalledState state = InstalledState::Absent;
    /// 首个不匹配的文件（相对文件名），无则为空。
    std::string badFile;
    /// 人类可读诊断。
    std::string detail;
    /// true 表示校验被取消（此时 state 无意义，调用方必须按"未知"处理）。
    bool cancelled = false;
};

/// 校验 versionDir 下的清单文件是否与内置摘要逐一相符。
///
/// cancelled 非空时会在逐文件哈希期间轮询；一旦返回 true 立即中止并置
/// `cancelled=true`——调用方**不得**把它当作 valid 或 tampered。
///
/// 受控路径约束：若 versionDir 位于 DataRootDir() 之下，会先校验
/// `<id>/<version>` 每一级都是真实目录（拒绝中间级符号链接），
/// 防止受控目录之外的内容被当成模型加载。
InstalledCheck CheckInstalledModel(const ModelSpec& spec,
                                   const std::string& versionDir,
                                   const std::function<bool()>& cancelled = {});

/// 在版本目录内写入 `install.json`（原子：tmp + rename）。
///
/// 该文件是**不可信 marker**，仅供快速否定与人类排查；任何校验都必须回到
/// CheckInstalledModel()/内置清单，绝不能读取本文件的摘要作为依据。
/// 写入内容自带 `"trusted": false`。
bool WriteInstallMarker(const ModelSpec& spec, const std::string& versionDir);

} // namespace fcitx::model_manager
