#pragma once
#include "Secret.h"
#include "RdpTarget.h"

// Nonsecret presentation state. No decrypted value or password length.
struct PasswordState {
    QString source;
    bool local = false, inherited = false, present = false, authorized = false;
};
struct PasswordAuthorization { QString consumer; SessionTarget target; };

// Owned C++ channel only: never a QVariant, JSON value or Qt signal argument.
struct PasswordEdit {
    enum class Kind { Set, Clear, Inheritance };
    Kind kind = Kind::Clear;
    Secret value;
    bool inherited = false;
    PasswordEdit clone() const { return {kind, value.clone(), inherited}; }
};
