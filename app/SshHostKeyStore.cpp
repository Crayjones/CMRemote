#include "SshHostKeyStore.h"
#include "remotemanager-core/src/ssh.rs.h"
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace {
constexpr qsizetype cacheBytes = 1024 * 1024;
QString text(const rust::String& value) { return QString::fromUtf8(value.data(), qsizetype(value.size())); }
rust::Vec<uint8_t> bytes(const QByteArray& value) {
    rust::Vec<uint8_t> result; result.reserve(size_t(value.size()));
    for (auto byte : value) result.push_back(uint8_t(byte)); return result;
}
SshHostKey fromRust(const rm::ssh::HostKeyIdentity& value) {
    return {text(value.host), value.port, text(value.algorithm),
        QByteArray(reinterpret_cast<const char*>(value.key.data()), qsizetype(value.key.size())), text(value.fingerprint)};
}
rm::ssh::HostKeyIdentity toRust(const SshHostKey& value) {
    return {value.host.toUtf8().toStdString(), value.port, value.algorithm.toUtf8().toStdString(), bytes(value.key), value.fingerprint.toUtf8().toStdString()};
}
SshHostKeyReply failure(QString error, bool conflict = false) {
    SshHostKeyReply reply; reply.result.error = std::move(error); reply.conflict = conflict; return reply;
}
bool endpoint(const SshHostKey& a, const SshHostKey& b) { return a.host == b.host && a.port == b.port && a.algorithm == b.algorithm; }
}
SshHostKey makeSshHostKey(const QString& host, quint16 port, const QByteArray& key) {
    if (host.size() > 4096 || key.size() > 16 * 1024) throw std::runtime_error("The SSH server key is unsupported or malformed.");
    return fromRust(rm::ssh::host_key_identity(host.toUtf8().toStdString(), port, bytes(key)));
}
SshHostKeyStore::SshHostKeyStore(QString path) : file_(std::move(path)) {}
void SshHostKeyStore::setFault(AtomicFile::Fault fault) { file_.setFault(fault); }
void SshHostKeyStore::setCommitGate(AtomicFile::CommitGate gate) { file_.setCommitGate(std::move(gate)); }
void SshHostKeyStore::cancel(const QString& ticket) { pending_.remove(ticket); }
FileResult SshHostKeyStore::load() {
    records_.clear(); QByteArray data; bool exists = false;
    if (!file_.load(data, exists, cacheBytes)) return {false, "The SSH host-key cache could not be read safely.", {}};
    if (exists) {
        try {
            const auto decoded = rm::ssh::decode_host_keys(rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t*>(data.constData()), size_t(data.size())));
            for (const auto& record : decoded) records_.push_back(fromRust(record));
        } catch (const std::exception&) { return {false, "The SSH host-key cache is malformed or exceeds supported limits.", {}}; }
    }
    return {true, {}, {}};
}
SshHostKeyReply SshHostKeyStore::inspect(const SshHostKey& candidate) {
    try {
        if (!(makeSshHostKey(candidate.host, candidate.port, candidate.key) == candidate)) return failure("The SSH server key is unsupported or malformed.");
    } catch (const std::exception&) { return failure("The SSH server key is unsupported or malformed."); }
    if (pending_.size() >= 1024) return failure("Too many pending SSH host-key requests.");
    const auto result = load(); if (!result.ok) { SshHostKeyReply reply; reply.result = result; return reply; }
    SshHostKeyInspection inspection; inspection.ticket = QUuid::createUuid().toString(QUuid::WithoutBraces); inspection.candidate = candidate;
    const auto existing = std::find_if(records_.begin(), records_.end(), [&](const auto& record) { return endpoint(record, candidate); });
    if (existing != records_.end()) {
        inspection.previous = *existing; inspection.status = *existing == candidate ? SshHostKeyStatus::Trusted : SshHostKeyStatus::Changed;
    }
    pending_.insert(inspection.ticket, {inspection, file_.revision(), file_.exists(), false, {}});
    SshHostKeyReply reply; reply.result = result; reply.inspection = std::move(inspection); return reply;
}
SshHostKeyReply SshHostKeyStore::decide(const QString& ticket, SshHostKeyDecision decision) {
    auto found = pending_.find(ticket);
    if (found == pending_.end()) return failure("Stale SSH host-key request.", true);
    const auto pending = *found; SshHostKeyReply reply;
    reply.inspection = pending.inspection; reply.committed = pending.committed;
    if (decision == SshHostKeyDecision::Cancel) {
        pending_.erase(found); reply.result = {true, {}, pending.warning}; reply.canceled = true; return reply;
    }
    reply.result = load(); if (!reply.result.ok) return reply;
    if (file_.revision() != pending.revision || file_.exists() != pending.exists || !file_.unchanged(cacheBytes)) {
        pending_.remove(ticket); reply.result = {false, "The SSH host-key cache changed. Check the server key again.", {}}; reply.conflict = true; return reply;
    }
    if (!pending.warning.isEmpty()) {
        if (decision != SshHostKeyDecision::AcknowledgeWarning) {
            reply.result = {false, "Acknowledge the committed SSH host-key save warning before connecting.", pending.warning}; return reply;
        }
        pending_.remove(ticket); reply.allowed = true; return reply;
    }
    if (decision == SshHostKeyDecision::ConnectOnce ||
        (decision == SshHostKeyDecision::UseTrusted && pending.inspection.status == SshHostKeyStatus::Trusted)) {
        pending_.remove(ticket); reply.allowed = true; return reply;
    }
    if (!((decision == SshHostKeyDecision::TrustAndSave && pending.inspection.status == SshHostKeyStatus::Unknown) ||
          (decision == SshHostKeyDecision::ReplaceAndConnect && pending.inspection.status == SshHostKeyStatus::Changed))) {
        reply.result = {false, "This SSH host-key decision does not match the inspected key.", {}}; return reply;
    }
    const auto& candidate = pending.inspection.candidate;
    auto existing = std::find_if(records_.begin(), records_.end(), [&](const auto& record) { return endpoint(record, candidate); });
    if (existing != records_.end()) *existing = candidate; else records_.push_back(candidate);
    QByteArray data;
    try {
        rust::Vec<rm::ssh::HostKeyIdentity> records;
        for (const auto& record : records_) records.push_back(toRust(record));
        const auto encoded = rm::ssh::encode_host_keys(records); data = QByteArray(encoded.data(), qsizetype(encoded.size()));
    } catch (const std::exception&) { reply.result = {false, "The SSH host-key cache exceeds supported limits.", {}}; return reply; }
    reply.result = file_.replace(data, cacheBytes, "SSH host-key cache");
    if (!reply.result.ok) {
        if (!file_.unchanged(cacheBytes)) { pending_.remove(ticket); reply.conflict = true; }
        return reply;
    }
    reply.committed = true;
    if (reply.result.warning.isEmpty()) { pending_.remove(ticket); reply.allowed = true; }
    else {
        auto& updated = pending_[ticket]; updated.revision = file_.revision(); updated.exists = true;
        updated.committed = true; updated.warning = reply.result.warning;
    }
    return reply;
}
