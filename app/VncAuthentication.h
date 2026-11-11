#pragma once
#include "Secret.h"
#include <QString>
#include <optional>

// Session-only authentication: never retained in targets, Core, or XML metadata.
// A saved password is sent only if the server asks for one (VncAuth, or the
// VeNCrypt Plain types together with a username); otherwise the session asks.
// Servers offering no authentication never receive a password.
struct VncAuthentication {
    static constexpr size_t MaxPassword = 4096;
    std::optional<Secret> password;
};
// Throws std::invalid_argument for an empty, oversized or NUL-containing password.
VncAuthentication vncPasswordAuthentication(Secret password);
VncAuthentication vncPromptAuthentication();
