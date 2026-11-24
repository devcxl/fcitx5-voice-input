#include <iostream>
#include <string>

#include <fcitx-config/rawconfig.h>

#include "config/voiceinput-config.h"

namespace {

bool ContainsValue(const fcitx::RawConfig& node, const std::string& value) {
    if (node.name() == value || node.value() == value) return true;
    for (const auto& name : node.subItems()) {
        const auto child = node.get(name);
        if (child && ContainsValue(*child, value)) return true;
    }
    return false;
}

template <typename Config>
bool HasManager(const std::string& uri) {
    Config config;
    fcitx::RawConfig description;
    config.dumpDescription(description);
    const bool shown = ContainsValue(description, uri) &&
                       !ContainsValue(description, "ModelDir");
    config.modelManager.setHidden(true);
    fcitx::RawConfig hiddenDescription;
    config.dumpDescription(hiddenDescription);
    return shown && !ContainsValue(hiddenDescription, uri);
}

} // namespace

int main() {
    const bool online = HasManager<fcitx::SherpaOnnxAsrConfig>(
        "fcitx://config/addon/voiceinput/model/online");
    const bool offline = HasManager<fcitx::SherpaOfflineAsrConfig>(
        "fcitx://config/addon/voiceinput/model/offline");
    if (!online || !offline) {
        std::cerr << "model manager URI missing from backend description\n";
        return 1;
    }
    return 0;
}
