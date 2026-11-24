#pragma once

// 受控模型的逐文件校验（addon 运行时，只读）。
//
// 权威来源只有一个：编译期内置的 model_manager 清单（model_catalog.h）。
// 版本目录里任何自报信息（install.json、文件名、大小写差异、目录名）都不能
// 作为"已正确安装"的证据，只能作为"肯定没装好"的快速否定。
//
// 本层是纯函数：不读环境变量、不扫描目录、不做前缀匹配。调用方给出
// 「清单项 + 版本目录绝对路径」，这里按 relativePath 精确打开每个文件，
// 逐字节比对 SHA-256 与大小。

#include <string>

#include "model_catalog.h"

namespace fcitx {

/// 本地模型可用性判定结果。
///
/// 分类的目的不是给用户看错误码，而是让 addon 在调用 sherpa 之前就能把
/// 「没装」「装坏了」「清单自身不合法」三种情况分开：前两种都返回 false
/// 关闭本地识别，绝不能把未验证的文件交给 sherpa（后者会 _Exit 或 SIGSEGV）。
enum class SherpaModelStatus {
    /// 全部文件存在且摘要、尺寸与清单一致。
    Ok,
    /// 版本目录或必需文件不存在：尚未安装。
    NotInstalled,
    /// 文件存在但内容与清单不符：被截断、替换或篡改（含非普通文件）。
    Tampered,
    /// 文件是普通文件但无法读取（权限、I/O 错误，或读取期间被改写）。
    Unreadable,
    /// 无法确定用户数据目录（XDG_DATA_HOME 非法且 HOME 不可用）。
    DataDirUnavailable,
    /// 清单项本身不可用（relativePath 不安全、角色未知、清单为空、
    /// 或清单自报的 streaming 属性与请求的后端不一致）。
    InvalidSpec,
};

/// 稳定的英文名，用于日志与测试断言。
const char* SherpaModelStatusName(SherpaModelStatus status);

/// 单个版本目录的校验结果。
struct SherpaModelVerification {
    SherpaModelStatus status = SherpaModelStatus::NotInstalled;
    /// 失败时指向清单里的 relativePath（诊断用，不含用户可控路径）。
    std::string failedRelativePath;
    /// 校验通过时填充为版本目录下的绝对路径；失败时为空。
    std::string encoder;
    std::string decoder;
    std::string joiner;
    std::string tokens;

    bool ok() const { return status == SherpaModelStatus::Ok; }
};

/// 用清单内置的 SHA-256 与尺寸校验 versionDir 中该模型的每个文件。
///
/// 受控链（重要）：versionDir 必须严格等于 `modelsRoot/<spec.id>/<spec.version>`，
/// 且从 modelsRoot 起每一级（modelsRoot 自身、`<id>`、`<version>`）都必须是**真实
/// 目录**——任何一级是符号链接都判为 Tampered。只在最终文件上用 O_NOFOLLOW 是不够的：
/// 若 `models/<id>` 是指向别处的符号链接，整个"受控目录"就被重定向了。
///
/// modelsRoot 之上（XDG_DATA_HOME、`fcitx5`、`voice-input`）允许是符号链接：
/// 那是正常系统配置（如把 ~/.local/share 放到另一块盘），且不改变"受控子树
/// 从 models 开始"这一约定。
///
/// 只按清单的 relativePath 精确取文件；不做目录遍历、不接受相对路径之外的名字。
/// 符号链接、目录、FIFO 等非普通文件一律判为 Tampered：它们不是受控内容。
///
/// 语义边界（重要）：
///   - 版本目录里的 `install.json` 等自报信息**完全不参与**判定；它由安装器
///     写入，用户可改，只能当作线索而不是证据。
///   - **仍有的 TOCTOU 限制**：校验与 sherpa 打开文件之间仍存在时间窗（sherpa 按
///     路径打开，无法接收 fd；`O_NOFOLLOW` 只覆盖最终组件）。本校验防的是「落盘
///     内容不对、版本/后端换错、受控目录被中间级链接重定向、被动损坏」，不是对
///     本地可写用户的沙箱；能改模型目录的人同样能在校验后替换文件。
SherpaModelVerification VerifyModelVersionDir(
    const model_manager::ModelSpec& spec, const std::string& modelsRoot,
    const std::string& versionDir);

} // namespace fcitx
