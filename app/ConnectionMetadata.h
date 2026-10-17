#pragma once
#include "ConnectionCrypto.h"
#include "RdpTarget.h"

struct ConnectionBinding {
    QString consumer, owner;
    QByteArray passwordHash;
    SessionTarget target;
};
struct ConnectionBindings { QString fileId; std::vector<ConnectionBinding> records; quint32 version = 1; };

// Private version-1 RDP and version-2 protocol-scoped formats. No secret-bearing JSON or diagnostic text.
namespace ConnectionMetadata {
constexpr qsizetype MaxPayload = 8 * 1024 * 1024;
using Canceled = std::function<bool()>;
std::optional<Secret> encodeBindings(const ConnectionBindings& bindings, const Canceled& canceled = {});
std::optional<ConnectionBindings> decodeBindings(const Secret& payload, const QString& fileId, const Canceled& canceled = {});
bool matches(const ConnectionBindings& bindings, const QString& fileId, const QString& consumer, const QString& owner,
             const QByteArray& encryptedPassword, const SessionTarget& committedTarget);
}
