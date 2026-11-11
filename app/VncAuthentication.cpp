#include "VncAuthentication.h"
#include <algorithm>
#include <stdexcept>

VncAuthentication vncPasswordAuthentication(Secret password) {
    // LibVNCClient passes passwords as C strings; VncAuth itself uses only the first 8 bytes.
    const bool valid = password.size() >= 1 && password.size() <= VncAuthentication::MaxPassword &&
        std::none_of(password.data(), password.data() + password.size(), [](unsigned char byte) { return byte == 0; });
    if (!valid) { password.clear(); throw std::invalid_argument("VNC passwords must be 1–4096 bytes without NUL characters."); }
    VncAuthentication result; result.password.emplace(std::move(password)); return result;
}
VncAuthentication vncPromptAuthentication() { return {}; }
