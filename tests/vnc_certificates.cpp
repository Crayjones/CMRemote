#include "VncCertificates.h"
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <memory>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

namespace {
struct KeyFree { void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); } };
struct X509Free { void operator()(X509* certificate) const { X509_free(certificate); } };
using Key = std::unique_ptr<EVP_PKEY, KeyFree>;
using Certificate = std::unique_ptr<X509, X509Free>;
struct Issued { Key key; Certificate certificate; QByteArray der; };

bool extension(X509* certificate, X509* issuer, int nid, const QByteArray& value) {
    X509V3_CTX context; X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
    auto* added = X509V3_EXT_conf_nid(nullptr, &context, nid, value.constData());
    const bool ok = added && X509_add_ext(certificate, added, -1) == 1;
    X509_EXTENSION_free(added); return ok;
}
// A synthetic certificate: a CA when names is empty, otherwise a TLS server certificate.
Issued issue(const QByteArray& commonName, const Issued* issuer, const QByteArray& names = {}, long fromSeconds = -3600, long toSeconds = 86400) {
    Issued result{Key(EVP_EC_gen("P-256")), Certificate(X509_new()), {}};
    auto* certificate = result.certificate.get();
    X509_set_version(certificate, 2);
    static long serial = 1; ASN1_INTEGER_set(X509_get_serialNumber(certificate), ++serial);
    X509_gmtime_adj(X509_getm_notBefore(certificate), fromSeconds); X509_gmtime_adj(X509_getm_notAfter(certificate), toSeconds);
    X509_NAME_add_entry_by_txt(X509_get_subject_name(certificate), "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>(commonName.constData()), -1, -1, 0);
    X509_set_issuer_name(certificate, issuer ? X509_get_subject_name(issuer->certificate.get()) : X509_get_subject_name(certificate));
    X509_set_pubkey(certificate, result.key.get());
    auto* signer = issuer ? issuer->certificate.get() : certificate;
    if (names.isEmpty()) {
        extension(certificate, signer, NID_basic_constraints, "critical,CA:TRUE");
        extension(certificate, signer, NID_key_usage, "critical,keyCertSign,cRLSign");
    } else {
        extension(certificate, signer, NID_basic_constraints, "CA:FALSE");
        extension(certificate, signer, NID_subject_alt_name, names);
        extension(certificate, signer, NID_ext_key_usage, "serverAuth");
    }
    X509_sign(certificate, issuer ? issuer->key.get() : result.key.get(), EVP_sha256());
    const int size = i2d_X509(certificate, nullptr); result.der.resize(size);
    auto* out = reinterpret_cast<unsigned char*>(result.der.data()); i2d_X509(certificate, &out);
    return result;
}
QByteArray pem(const QByteArray& der) {
    return "-----BEGIN CERTIFICATE-----\n" + der.toBase64() + "\n-----END CERTIFICATE-----\n";
}
}

class VncCertificatesTest : public QObject {
    Q_OBJECT
    Issued root_ = issue("Synthetic VNC root", nullptr);
private slots:
    void readsPemBundlesAndSingleDerOnly() {
        const auto server = issue("vnc.test", &root_, "DNS:vnc.test");
        QCOMPARE(readVncCertificates(pem(root_.der) + "comment\n" + pem(server.der)), (QList<QByteArray>{root_.der, server.der}));
        QCOMPARE(readVncCertificates(server.der), QList<QByteArray>{server.der});
        QVERIFY(readVncCertificates(server.der + "x").isEmpty()); // trailing bytes are not one certificate
        QVERIFY(readVncCertificates("not a certificate").isEmpty());
        const auto described = describeVncCertificate(server.der); QVERIFY(described);
        QCOMPARE(described->subject, QString("CN=vnc.test")); QCOMPARE(described->issuer, QString("CN=Synthetic VNC root"));
        QCOMPARE(described->name(), QString("vnc.test")); QVERIFY(!described->authority); QVERIFY(describeVncCertificate(root_.der)->authority);
        QCOMPARE(described->sha256.size(), 32); QCOMPARE(described->fingerprint().size(), 32 * 3 - 1);
        QVERIFY(described->notBefore < QDateTime::currentDateTimeUtc() && described->notAfter > QDateTime::currentDateTimeUtc());
    }
    void verifiesImportedAuthorityHostNamesAndAddresses() {
        const auto server = issue("vnc.test", &root_, "DNS:vnc.test,IP:10.1.2.3");
        QVERIFY(verifyVncCertificate("vnc.test", server.der, {}, {root_.der}).trusted);
        QVERIFY(verifyVncCertificate("10.1.2.3", server.der, {}, {root_.der}).trusted);
        const auto otherName = verifyVncCertificate("other.test", server.der, {}, {root_.der});
        QVERIFY(!otherName.trusted); QVERIFY2(otherName.reason.contains("mismatch"), qPrintable(otherName.reason));
        const auto otherAddress = verifyVncCertificate("10.1.2.4", server.der, {}, {root_.der});
        QVERIFY(!otherAddress.trusted); QVERIFY2(otherAddress.reason.contains("mismatch"), qPrintable(otherAddress.reason));
        // Never trusted without the authority (it is in no system store).
        const auto unknown = verifyVncCertificate("vnc.test", server.der, {}, {});
        QVERIFY(!unknown.trusted); QVERIFY(!unknown.reason.isEmpty());
    }
    void rejectsExpiredSelfSignedAndCaLeafCertificates() {
        const auto expired = issue("vnc.test", &root_, "DNS:vnc.test", -7200, -3600);
        const auto result = verifyVncCertificate("vnc.test", expired.der, {}, {root_.der});
        QVERIFY(!result.trusted); QVERIFY2(result.reason.contains("expired"), qPrintable(result.reason));
        const auto self = issue("vnc.test", nullptr);
        QVERIFY(!verifyVncCertificate("vnc.test", self.der, {}, {}).trusted);
        QVERIFY(!verifyVncCertificate("vnc.test", "garbage", {}, {root_.der}).trusted);
    }
    void usesTheChainTheServerSent() {
        const auto intermediate = issue("Synthetic VNC intermediate", &root_);
        const auto server = issue("vnc.test", &intermediate, "DNS:vnc.test");
        QVERIFY(verifyVncCertificate("vnc.test", server.der, {intermediate.der}, {root_.der}).trusted);
        QVERIFY(!verifyVncCertificate("vnc.test", server.der, {}, {root_.der}).trusted);
        // An intermediate is not a trust anchor on its own.
        QVERIFY(!verifyVncCertificate("vnc.test", server.der, {}, {intermediate.der}).trusted);
    }
    void storeImportsAuthoritiesPinsServersAndPersists() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        // The store refuses symlinked parents, and the system temporary folder may be one.
        const auto path = QFileInfo(directory.path()).canonicalFilePath() + "/vnc-certificates.json";
        const auto server = issue("vnc.test", &root_, "DNS:vnc.test");
        VncTrustStore store(path); QVERIFY(store.load().ok); QVERIFY(store.authorities().isEmpty());
        int added = -1;
        QVERIFY(store.importAuthorities(pem(root_.der), &added).ok); QCOMPARE(added, 1);
        QVERIFY(store.importAuthorities(root_.der, &added).ok); QCOMPARE(added, 0); // already imported
        const auto leaf = store.importAuthorities(pem(server.der), &added);
        QVERIFY(!leaf.ok); QVERIFY(leaf.error.contains("not a certificate authority")); QCOMPARE(store.authorities().size(), 1);
        QVERIFY(!store.importAuthorities("nothing", &added).ok);
        QVERIFY(store.pin("VNC.test", 5903, server.der).ok);
        QVERIFY(store.pinned("vnc.test", 5903)); QCOMPARE(store.pinned("vnc.test", 5903)->der, server.der);
        QVERIFY(!store.pinned("vnc.test", 5904)); QVERIFY(!store.pinned("other.test", 5903));
        // Pinning again replaces the certificate for that endpoint.
        const auto replacement = issue("vnc.test", &root_, "DNS:vnc.test");
        QVERIFY(store.pin("vnc.test", 5903, replacement.der).ok); QCOMPARE(store.servers().size(), 1);
        VncTrustStore reopened(path); QVERIFY(reopened.load().ok);
        QCOMPARE(reopened.authorityCertificates(), QList<QByteArray>{root_.der});
        QCOMPARE(reopened.pinned("vnc.test", 5903)->der, replacement.der);
        // A change saved by another window is kept by the next edit here.
        QVERIFY(reopened.unpin("vnc.test", 5903).ok);
        QVERIFY(store.removeAuthority(describeVncCertificate(root_.der)->sha256).ok);
        QVERIFY(store.authorities().isEmpty()); QVERIFY(store.servers().isEmpty());
    }
    void invalidFileIsReportedAndNeverOverwritten() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        // The store refuses symlinked parents, and the system temporary folder may be one.
        const auto path = QFileInfo(directory.path()).canonicalFilePath() + "/vnc-certificates.json";
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("{\"version\": 1, \"authorities\": [\"@@\"], \"servers\": []}"); }
        VncTrustStore store(path);
        QVERIFY(!store.load().ok);
        QVERIFY(!store.importAuthorities(pem(root_.der)).ok);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(file.readAll().contains("@@"));
    }
    void memoryStoreNeedsNoFile() {
        VncTrustStore store({}); QVERIFY(store.load().ok);
        QVERIFY(store.importAuthorities(pem(root_.der)).ok); QCOMPARE(store.authorities().size(), 1);
        QVERIFY(store.pin("vnc.test", 5900, issue("vnc.test", &root_, "DNS:vnc.test").der).ok); QVERIFY(store.pinned("vnc.test", 5900));
    }
};
QTEST_MAIN(VncCertificatesTest)
#include "vnc_certificates.moc"
