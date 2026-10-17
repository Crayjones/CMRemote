#include "ConnectionMetadata.h"
#include <QCryptographicHash>
#include <QTest>
#include <QtEndian>
#include <QUuid>

namespace {
const QString FileId = "f6595425-c35a-45b7-9dfc-84dd83e6b002", Profile = "e6683f1f-4e06-4200-9a17-89cd30c68b12", Other = "1897a1fb-b702-45e9-bcae-884f59cc66c7";
QByteArray view(const Secret& value) { return QByteArray::fromRawData(reinterpret_cast<const char*>(value.data()), qsizetype(value.size())); }
void number(QByteArray& out, quint32 n) { const auto be = qToBigEndian(n); out.append(reinterpret_cast<const char*>(&be), 4); }
void field(QByteArray& out, const QByteArray& value) { number(out, quint32(value.size())); out += value; }
SessionTarget target() { return {"localhost", 3389, "fixture-user", "EXAMPLE"}; }
ConnectionBindings bindings() { return {FileId, {{Profile, Profile, QByteArray(32, 'h'), target()}}}; }
QByteArray expectedBinding() {
    // Independent construction of the SPEC binary order, not the codec writer.
    QByteArray raw("RMBIND\0\0", 8); number(raw, 1); raw += QUuid(FileId).toRfc4122(); number(raw, 1);
    raw += QUuid(Profile).toRfc4122(); raw += QUuid(Profile).toRfc4122(); raw += QByteArray(32, 'h'); raw += char(1);
    field(raw, "localhost"); raw += QByteArray::fromHex("0d3d"); field(raw, "fixture-user"); field(raw, "EXAMPLE"); return raw;
}
}
class ConnectionMetadataTest : public QObject {
    Q_OBJECT
private slots:
    void bindingBinaryLayoutAndBounds() {
        auto original = bindings(); auto encoded = ConnectionMetadata::encodeBindings(original); QVERIFY(encoded);
        QVERIFY(view(*encoded) == expectedBinding());
        const auto reopened = ConnectionMetadata::decodeBindings(*encoded, FileId); QVERIFY(reopened);
        QCOMPARE(reopened->fileId, FileId); QCOMPARE(reopened->records.size(), size_t(1));
        QCOMPARE(reopened->records[0].consumer, Profile); QCOMPARE(reopened->records[0].owner, Profile);
        QVERIFY(reopened->records[0].target.sameAccount(target()));
        QVERIFY(!ConnectionMetadata::decodeBindings(*encoded, Other));
        QVERIFY(!ConnectionMetadata::encodeBindings(original, [] { return true; }));
        QVERIFY(!ConnectionMetadata::decodeBindings(*encoded, FileId, [] { return true; }));
        auto invalid = original; invalid.fileId = "bad"; QVERIFY(!ConnectionMetadata::encodeBindings(invalid));
        invalid = original; invalid.records[0].passwordHash.resize(31); QVERIFY(!ConnectionMetadata::encodeBindings(invalid));
        invalid = original; invalid.records[0].target.host = " LOCALHOST "; QVERIFY(!ConnectionMetadata::encodeBindings(invalid));
        invalid = original; invalid.records[0].target.username = QString(1025, 'u'); QVERIFY(!ConnectionMetadata::encodeBindings(invalid));
        invalid = original; invalid.records[0].target.domain = QString(257, 'd'); QVERIFY(!ConnectionMetadata::encodeBindings(invalid));
        invalid = original; invalid.records[0].target.port = 0; QVERIFY(!ConnectionMetadata::encodeBindings(invalid));
        invalid = original; invalid.records.push_back(invalid.records[0]); QVERIFY(!ConnectionMetadata::encodeBindings(invalid));
        invalid = original; invalid.records.resize(1025, invalid.records[0]); QVERIFY(!ConnectionMetadata::encodeBindings(invalid));
        auto tooMany = expectedBinding(); const auto n = qToBigEndian(quint32(1025)); tooMany.replace(28, 4, reinterpret_cast<const char*>(&n), 4);
        auto protocol = expectedBinding(); protocol[96] = char(2);
        auto oversized = expectedBinding(); const auto large = qToBigEndian(quint32(1025)); oversized.replace(97, 4, reinterpret_cast<const char*>(&large), 4);
        auto version = expectedBinding(); version[11] = char(4);
        for (const auto& bad : {tooMany, protocol, oversized, version, expectedBinding().left(31), expectedBinding() + 'x', QByteArray(ConnectionMetadata::MaxPayload + 1, 'x')})
            QVERIFY(!ConnectionMetadata::decodeBindings(Secret(bad), FileId));
        ConnectionBindings two{FileId, {{Profile, Profile, QByteArray(32, 'h'), target()}, {Other, Profile, QByteArray(32, 'h'), target()}}};
        auto sorted = ConnectionMetadata::encodeBindings(two); QVERIFY(sorted); auto decoded = ConnectionMetadata::decodeBindings(*sorted, FileId); QVERIFY(decoded);
        QCOMPARE(decoded->records.front().consumer, Other);
        ConnectionBindings empty{FileId, {}}; auto none = ConnectionMetadata::encodeBindings(empty); QVERIFY(none);
        QVERIFY(ConnectionMetadata::decodeBindings(*none, FileId)->records.empty());
    }
    void sshBindingV2IsProtocolScopedAndV1RdpStillReads() {
        auto ssh = sessionTarget(QJsonObject{{"protocol", "ssh"}, {"host", "localhost"}, {"port", 22},
            {"username", "TEAM\\literal-user"}, {"ssh_auth", "password"}, {"ssh_key_path", ""}});
        ConnectionBindings saved{FileId, {{Profile, Profile, QByteArray(32, 'h'), ssh}, {Other, Other, QByteArray(32, 'r'), target()}}};
        auto encoded = ConnectionMetadata::encodeBindings(saved); QVERIFY(encoded);
        QCOMPARE(qFromBigEndian<quint32>(encoded->data() + 8), quint32(2));
        const auto decoded = ConnectionMetadata::decodeBindings(*encoded, FileId); QVERIFY(decoded);
        QCOMPARE(decoded->records.size(), size_t(2));
        QVERIFY(decoded->records[1].target.sameAccount(ssh));
        auto v1 = ConnectionMetadata::decodeBindings(Secret(expectedBinding()), FileId); QVERIFY(v1);
        auto unchanged = ConnectionMetadata::encodeBindings(*v1); QVERIFY(unchanged); QCOMPARE(view(*unchanged), expectedBinding());
        auto rdpOnSameEndpoint = ssh; rdpOnSameEndpoint = sessionTarget(QJsonObject{{"protocol", "rdp"},
            {"host", "localhost"}, {"port", 22}, {"username", "TEAM\\literal-user"}});
        QVERIFY(!ssh.sameAccount(rdpOnSameEndpoint));
        auto wrongVersion = view(*encoded); wrongVersion[11] = char(1);
        QVERIFY(!ConnectionMetadata::decodeBindings(Secret(wrongVersion), FileId));
        auto privateKey = sessionTarget(QJsonObject{{"protocol", "ssh"}, {"host", "localhost"}, {"port", 22},
            {"username", "TEAM\\literal-user"}, {"ssh_auth", "private_key"}, {"ssh_key_path", "/owned/id_ed25519"}});
        saved.records[0].target = privateKey; QVERIFY(!ConnectionMetadata::encodeBindings(saved));
        auto noncanonicalDomain = ssh; noncanonicalDomain.domain = "inactive";
        saved.records[0].target = noncanonicalDomain; QVERIFY(!ConnectionMetadata::encodeBindings(saved));
        auto unknown = view(*encoded); unknown[96] = char(3);
        QVERIFY(!ConnectionMetadata::decodeBindings(Secret(unknown), FileId));
        auto canonical = normalizeSshTarget(sessionTarget(QJsonObject{{"protocol", "ssh"}, {"host", "LOCALHOST"}, {"port", 22},
            {"username", "TEAM\\literal-user"}, {"domain", "passive /@ domain"}, {"ssh_auth", "password"}}));
        QVERIFY(canonical); QCOMPARE(canonical->host, QString("localhost")); QVERIFY(canonical->domain.isEmpty());
        QVERIFY(canonical->sameAccount(ssh));
    }
    void vncBindingV3IsHostAndPortOnly() {
        auto vnc = sessionTarget(QJsonObject{{"protocol", "vnc"}, {"host", "parrot.example"}, {"port", 5901}, {"view_only", true}});
        QVERIFY(vnc.username.isEmpty());
        ConnectionBindings saved{FileId, {{Profile, Profile, QByteArray(32, 'v'), vnc}, {Other, Other, QByteArray(32, 'r'), target()}}};
        auto encoded = ConnectionMetadata::encodeBindings(saved); QVERIFY(encoded);
        QCOMPARE(qFromBigEndian<quint32>(encoded->data() + 8), quint32(3));
        const auto decoded = ConnectionMetadata::decodeBindings(*encoded, FileId); QVERIFY(decoded);
        const auto& record = decoded->records[0].consumer == Profile ? decoded->records[0] : decoded->records[1];
        QCOMPARE(record.target.protocol, QString("vnc")); QCOMPARE(record.target.port, quint16(5901));
        QVERIFY(record.target.sameAccount(vnc)); QVERIFY(record.target.username.isEmpty());
        // View only is not part of the account; protocol and endpoint are.
        auto viewOnly = vnc; viewOnly.viewOnly = false; QVERIFY(record.target.sameAccount(viewOnly));
        auto otherPort = vnc; otherPort.port = 5902; QVERIFY(!record.target.sameAccount(otherPort));
        auto rdp = sessionTarget(QJsonObject{{"protocol", "rdp"}, {"host", "parrot.example"}, {"port", 5901}, {"username", "user"}});
        QVERIFY(!record.target.sameAccount(rdp));
        auto withUser = vnc; withUser.username = "inactive"; saved.records[0].target = withUser; QVERIFY(!ConnectionMetadata::encodeBindings(saved));
        auto older = view(*encoded); older[11] = char(2);
        QVERIFY(!ConnectionMetadata::decodeBindings(Secret(older), FileId));
    }
    void credentialBindingsRejectSwapsAndRequireImportConsent() {
        const Secret master(QByteArray("synthetic XML master"));
        const auto cipher = ConnectionCrypto::encrypt(master, 1000, Secret(QByteArray("synthetic credential")), 4096); QVERIFY(cipher);
        const auto replacement = ConnectionCrypto::encrypt(master, 1000, Secret(QByteArray("synthetic credential")), 4096); QVERIFY(replacement);
        auto saved = bindings(); saved.records[0].passwordHash = QCryptographicHash::hash(QByteArray::fromBase64(*cipher), QCryptographicHash::Sha256);
        QVERIFY(ConnectionMetadata::matches(saved, FileId, Profile, Profile, *cipher, target()));
        QVERIFY(!ConnectionMetadata::matches(saved, Other, Profile, Profile, *cipher, target()));
        QVERIFY(!ConnectionMetadata::matches(saved, FileId, Other, Profile, *cipher, target()));
        QVERIFY(!ConnectionMetadata::matches(saved, FileId, Profile, Other, *cipher, target()));
        QVERIFY(!ConnectionMetadata::matches(saved, FileId, Profile, Profile, *replacement, target()));
        auto changed = target(); changed.host = "other.test"; QVERIFY(!ConnectionMetadata::matches(saved, FileId, Profile, Profile, *cipher, changed));
        changed = target(); changed.port = 3390; QVERIFY(!ConnectionMetadata::matches(saved, FileId, Profile, Profile, *cipher, changed));
        changed = target(); changed.username = "different-user"; QVERIFY(!ConnectionMetadata::matches(saved, FileId, Profile, Profile, *cipher, changed));
        changed = target(); changed.domain = "OTHER"; QVERIFY(!ConnectionMetadata::matches(saved, FileId, Profile, Profile, *cipher, changed));
        QVERIFY(!ConnectionMetadata::matches(ConnectionBindings{FileId, {}}, FileId, Profile, Profile, *cipher, target()));
        QVERIFY(!ConnectionMetadata::matches(saved, FileId, Profile, Profile, {}, target()));
    }
};
QTEST_GUILESS_MAIN(ConnectionMetadataTest)
#include "connection_metadata.moc"
