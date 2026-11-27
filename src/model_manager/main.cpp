// voice-input-model-installer：受控模型安装器。
//
// 该可执行文件有三类入口：
//   1. 正常安装（默认）：下载/校验/解包/试加载/原子发布，stdout 输出 JSON Lines。
//   2. --verify-internal <id> <dir>：子进程试加载模式，由安装流程内部 spawn 自身。
//   3. --list：列出受控模型清单。

#include "cli.h"

int main(int argc, char** argv) {
    return fcitx::model_manager::RunInstallerCli(argc, argv);
}
