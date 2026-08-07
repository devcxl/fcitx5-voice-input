#include "base64.h"

namespace fcitx {

namespace {
const char b64table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
}

std::string Base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (static_cast<uint32_t>(data[i]) << 16)
                   | (i + 1 < len ? static_cast<uint32_t>(data[i + 1]) << 8 : 0)
                   | (i + 2 < len ? static_cast<uint32_t>(data[i + 2]) : 0);
        out.push_back(b64table[(v >> 18) & 0x3F]);
        out.push_back(b64table[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < len ? b64table[(v >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < len ? b64table[v & 0x3F] : '=');
    }
    return out;
}

} // namespace fcitx
