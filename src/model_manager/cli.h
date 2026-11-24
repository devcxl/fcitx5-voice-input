#pragma once

// 安装器命令行入口。
//
// 用法：
//   voice-input-model-installer --model <online-zipformer|offline-x-asr>
//                              [--archive <path>] [--no-trial] [--trial-timeout-ms N]
//   voice-input-model-installer --verify-internal <id> <versionDir>
//   voice-input-model-installer --list
//
// stdout 只输出 JSON Lines 协议事件（见 install_events.h）；日志与诊断走 stderr。
// 退出码：0 成功；2 参数错误；3 模型未知；其他非 0 表示对应安装失败。

namespace fcitx::model_manager {

int RunInstallerCli(int argc, char** argv);

} // namespace fcitx::model_manager
