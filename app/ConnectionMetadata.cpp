#include "ConnectionMetadata.h"
#include <QCryptographicHash>
#include <QSet>
#include <QUuid>
#include <QtEndian>
#include <openssl/crypto.h>
#include <algorithm>
#include <cstring>

namespace {
constexpr quint32 MaxRecords = 1024;
const QByteArray BindingMagic("RMBIND\0\0", 8);
bool stopped(const ConnectionMetadata::Canceled& canceled) { return canceled && canceled(); }
std::optional<QByteArray> uuid(const QString& value, bool canonical = false) {
    const QUuid id(value); if (id.isNull() || id.toString(QUuid::WithoutBraces) != (canonical ? value : value.toLower())) return std::nullopt;
    return id.toRfc4122();
}
bool text(const QString& value, qsizetype max) {
    const auto bytes = value.toUtf8(); if (bytes.size() > max || QString::fromUtf8(bytes) != value) return false;
    for (auto ch : value) if (ch.category() == QChar::Other_Control) return false;
    return true;
}
bool target(const SessionTarget& value) {
    if (!text(value.host, 1024) || !text(value.username, 1024) || !text(value.domain, 256) || value.port == 0) return false;
    auto account = value; account.options = {};
    if (account.protocol == "ssh" && (account.sshAuth != "password" || !account.domain.isEmpty())) return false;
    // VNC accounts are only host and port; no username or domain is ever bound.
    if (account.protocol == "vnc" && (!account.username.isEmpty() || !account.domain.isEmpty())) return false;
    const auto normalized = account.protocol == "ssh" ? normalizeSshTarget(account) :
        account.protocol == "vnc" ? normalizeVncTarget(account) : normalizeRdpTarget(account);
    return normalized && normalized->sameAccount(account);
}
qsizetype fieldSize(const QString& value) { return 4 + value.toUtf8().size(); }
// Binding format versions: 1 RDP, 2 adds SSH, 3 adds VNC.
quint8 protocolCode(const QString& protocol) { return protocol == "ssh" ? 2 : protocol == "vnc" ? 3 : 1; }
quint32 minimumVersion(const QString& protocol) { return protocol == "ssh" ? 2 : protocol == "vnc" ? 3 : 1; }
bool validVersion(quint32 version) { return version >= 1 && version <= 3; }
struct Writer {
    explicit Writer(qsizetype size) : data(size_t(size)) {}
    Secret data; qsizetype position = 0; bool ok = true;
    void raw(const void* bytes, qsizetype size) {
        if (!ok || size < 0 || size > qsizetype(data.size()) - position) { ok = false; return; }
        if (size) std::memcpy(data.data() + position, bytes, size_t(size)); position += size;
    }
    void raw(const QByteArray& bytes) { raw(bytes.constData(), bytes.size()); }
    void u32(quint32 value) { const auto be = qToBigEndian(value); raw(&be, 4); }
    void u16(quint16 value) { const auto be = qToBigEndian(value); raw(&be, 2); }
    void byte(quint8 value) { raw(&value, 1); }
    void field(const QString& value) { const auto bytes = value.toUtf8(); u32(quint32(bytes.size())); raw(bytes); }
    std::optional<Secret> finish() { if (!ok || position != qsizetype(data.size())) return std::nullopt; return std::move(data); }
};
struct Reader {
    explicit Reader(const Secret& value) : data(value.data()), left(qsizetype(value.size())) {}
    const unsigned char* data; qsizetype left;
    bool raw(QByteArray& bytes, qsizetype size) {
        if (size < 0 || size > left) return false;
        bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(data), size); data += size; left -= size; return true;
    }
    bool u32(quint32& value) { if (left < 4) return false; value = qFromBigEndian<quint32>(data); data += 4; left -= 4; return true; }
    bool u16(quint16& value) { if (left < 2) return false; value = qFromBigEndian<quint16>(data); data += 2; left -= 2; return true; }
    bool byte(quint8& value) { if (left < 1) return false; value = *data++; --left; return true; }
    bool id(QString& value) {
        QByteArray bytes; if (!raw(bytes, 16)) return false;
        const auto id = QUuid::fromRfc4122(bytes); if (id.isNull()) return false;
        value = id.toString(QUuid::WithoutBraces); return true;
    }
    bool field(QString& value, quint32 max) {
        quint32 size; QByteArray bytes; if (!u32(size) || size > max || !raw(bytes, size)) return false;
        value = QString::fromUtf8(bytes); return value.toUtf8() == bytes;
    }

};
}

std::optional<Secret> ConnectionMetadata::encodeBindings(const ConnectionBindings& bindings, const Canceled& canceled) {
    const auto file = uuid(bindings.fileId);
    if (!file || !validVersion(bindings.version) || bindings.records.size() > MaxRecords || stopped(canceled)) return std::nullopt;
    quint32 version = bindings.version;
    qsizetype size = 32; std::vector<const ConnectionBinding*> sorted; sorted.reserve(bindings.records.size());
    QSet<QString> consumers;
    for (const auto& record : bindings.records) {
        if (stopped(canceled) || !uuid(record.consumer) || !uuid(record.owner) || record.passwordHash.size() != 32 || !target(record.target) || consumers.contains(record.consumer.toLower())) return std::nullopt;
        version = std::max(version, minimumVersion(record.target.protocol));
        consumers.insert(record.consumer.toLower()); sorted.push_back(&record);
        size += 16 + 16 + 32 + 1 + fieldSize(record.target.host) + 2 + fieldSize(record.target.username) + fieldSize(record.target.domain);
        if (size > MaxPayload) return std::nullopt;
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) { return a->consumer.toLower() < b->consumer.toLower(); });
    Writer writer(size); writer.raw(BindingMagic); writer.u32(version); writer.raw(*file); writer.u32(quint32(sorted.size()));
    for (const auto* record : sorted) {
        if (stopped(canceled)) return std::nullopt;
        writer.raw(*uuid(record->consumer)); writer.raw(*uuid(record->owner)); writer.raw(record->passwordHash); writer.byte(protocolCode(record->target.protocol));
        writer.field(record->target.host); writer.u16(record->target.port); writer.field(record->target.username); writer.field(record->target.domain);
    }
    if (stopped(canceled)) return std::nullopt; return writer.finish();
}
std::optional<ConnectionBindings> ConnectionMetadata::decodeBindings(const Secret& payload, const QString& fileId, const Canceled& canceled) {
    const auto expected = uuid(fileId); if (!expected || payload.size() > size_t(MaxPayload) || stopped(canceled)) return std::nullopt;
    Reader reader(payload); QByteArray magic; quint32 version, count; ConnectionBindings result;
    if (!reader.raw(magic, 8) || magic != BindingMagic || !reader.u32(version) || !validVersion(version) || !reader.id(result.fileId) ||
        *uuid(result.fileId) != *expected || !reader.u32(count) || count > MaxRecords) return std::nullopt;
    result.version = version; result.records.reserve(count); QString previous;
    for (quint32 i = 0; i < count; ++i) {
        if (stopped(canceled)) return std::nullopt;
        ConnectionBinding record; QByteArray hash; quint8 protocol;
        if (!reader.id(record.consumer) || (!previous.isEmpty() && previous >= record.consumer) || !reader.id(record.owner) || !reader.raw(hash, 32) ||
            !reader.byte(protocol) || protocol < 1 || protocol > 3 || version < quint32(protocol) || !reader.field(record.target.host, 1024) || !reader.u16(record.target.port) ||
            !reader.field(record.target.username, 1024) || !reader.field(record.target.domain, 256)) return std::nullopt;
        record.target.protocol = protocol == 3 ? "vnc" : protocol == 2 ? "ssh" : "rdp";
        if (!target(record.target)) return std::nullopt;
        // Reader views cannot outlive the owned plaintext payload.
        record.passwordHash = QByteArray(hash.constData(), hash.size()); previous = record.consumer; result.records.push_back(std::move(record));
    }
    if (reader.left != 0 || stopped(canceled)) return std::nullopt; return result;
}
bool ConnectionMetadata::matches(const ConnectionBindings& bindings, const QString& fileId, const QString& consumer, const QString& owner,
                                  const QByteArray& encryptedPassword, const SessionTarget& committedTarget) {
    const auto file = uuid(fileId), boundFile = uuid(bindings.fileId), user = uuid(consumer), source = uuid(owner);
    if (!validVersion(bindings.version) || bindings.version < minimumVersion(committedTarget.protocol) ||
        !file || !boundFile || *file != *boundFile || !user || !source || !target(committedTarget) || encryptedPassword.isEmpty() ||
        !ConnectionCrypto::validEnvelope(encryptedPassword, 4096)) return false;
    const auto hash = QCryptographicHash::hash(QByteArray::fromBase64(encryptedPassword), QCryptographicHash::Sha256);
    const ConnectionBinding* found = nullptr;
    for (const auto& record : bindings.records) {
        const auto recordConsumer = uuid(record.consumer);
        if (!recordConsumer || *recordConsumer != *user) continue;
        if (found) return false; found = &record;
    }
    if (!found || found->passwordHash.size() != 32 || !found->target.sameAccount(committedTarget)) return false;
    const auto recordOwner = uuid(found->owner);
    return recordOwner && *recordOwner == *source && CRYPTO_memcmp(found->passwordHash.constData(), hash.constData(), 32) == 0;
}
