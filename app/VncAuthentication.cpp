#include "VncAuthentication.h"
#include <stdexcept>

rust::Box<rm::vnc::Authentication> vncPasswordAuthentication(Secret password) {
    bool valid = password.size() >= 1 && password.size() <= 8;
    for (size_t i = 0; i < password.size(); ++i) valid = valid && password.data()[i] >= 0x20 && password.data()[i] <= 0x7e;
    if (!valid) throw std::invalid_argument("VNC password must contain 1–8 printable ASCII characters.");
    rust::Vec<uint8_t> bytes; bytes.reserve(password.size());
    for (size_t i = 0; i < password.size(); ++i) bytes.push_back(password.data()[i]);
    password.clear(); return rm::vnc::password_auth(std::move(bytes));
}
rust::Box<rm::vnc::Authentication> vncPromptAuthentication() { return rm::vnc::prompt_auth(); }
