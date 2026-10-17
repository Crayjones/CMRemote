#include "SshAuthentication.h"
#include <stdexcept>

namespace {
rust::Vec<uint8_t> transfer(Secret& secret) {
    if (secret.size() > 4096) throw std::runtime_error("SSH authentication must be at most 4096 UTF-8 bytes.");
    rust::Vec<uint8_t> bytes; bytes.reserve(secret.size());
    for (size_t i = 0; i < secret.size(); ++i) bytes.push_back(secret.data()[i]);
    secret.clear(); return bytes;
}
}
rust::Box<rm::ssh::Authentication> sshPasswordAuthentication(Secret password) {
    return rm::ssh::password_auth(transfer(password));
}
rust::Box<rm::ssh::Authentication> sshPrivateKeyAuthentication(const QString& path, Secret passphrase) {
    return rm::ssh::private_key_auth(path.toUtf8().toStdString(), transfer(passphrase));
}
