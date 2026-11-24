#pragma once

// 凭据配置文件权限的测试夹具。
//
// 目标：用真实落盘文件证明“保存后收紧为 0600”，并覆盖“文件已存在（且曾被外部
// 放宽）”的保存路径。所有路径通过 fcitx 自己的 StandardPaths(PkgConfig) 解析，
// 不猜目录；断言消息即失败原因，调用方原样输出。

#include <string>
#include <sys/stat.h>

#include "xdg_sandbox.h"

namespace test_support {

/// 主配置（不含凭据，但与凭据文件走同一条保存/收紧路径）。
inline std::string MainConfigPath() {
    return UserPkgConfigDir() + "/conf/voiceinput.conf";
}

/// 凭据子配置：用非活跃后端（volcengine）承载 API Key，写入不会触发引擎重载。
inline std::string CredentialSubConfigPath() {
    return UserPkgConfigDir() + "/conf/voiceinput-volcengine.conf";
}

/// 预置 0644 的占位文件，使随后的保存走“已存在文件”的覆盖路径。
inline bool SeedInsecureCredentials() {
    return SeedFileWithMode(MainConfigPath(), "", 0644) &&
           SeedFileWithMode(CredentialSubConfigPath(), "", 0644);
}

/// 校验：主配置与凭据子配置均为 0600，且凭据文件确实包含 expectedSecret。
/// 返回空串表示通过，否则返回失败原因。
inline std::string VerifyCredentialPermissions(
    const std::string& expectedSecret) {
    if (FilePermissions(MainConfigPath()) != 0600) {
        return "main config is not 0600 after save";
    }
    if (FilePermissions(CredentialSubConfigPath()) != 0600) {
        return "credential sub-config is not 0600 after save";
    }
    if (ReadFile(CredentialSubConfigPath()).find(expectedSecret) ==
        std::string::npos) {
        return "credential file does not contain the saved key";
    }
    return {};
}

/// 把凭据文件放宽为 0644，模拟权限被外部改动过。
inline bool RelaxCredentialPermissions() {
    return ::chmod(CredentialSubConfigPath().c_str(), 0644) == 0;
}

} // namespace test_support
