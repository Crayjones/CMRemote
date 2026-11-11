#include "VncCertificates.h"
#include <QCryptographicHash>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <algorithm>
#include <memory>
#include <openssl/asn1.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#ifdef Q_OS_MACOS
#include <Security/Security.h>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#endif

namespace {
struct X509Free { void operator()(X509* value) const { X509_free(value); } };
struct StoreFree { void operator()(X509_STORE* value) const { X509_STORE_free(value); } };
struct ContextFree { void operator()(X509_STORE_CTX* value) const { X509_STORE_CTX_free(value); } };
struct StackFree { void operator()(STACK_OF(X509)* value) const { sk_X509_pop_free(value, X509_free); } };
using X509Pointer = std::unique_ptr<X509, X509Free>;

X509Pointer parse(const QByteArray& der) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(der.constData());
    X509Pointer certificate(d2i_X509(nullptr, &bytes, long(der.size())));
    // Exactly one certificate and nothing after it.
    if (certificate && bytes != reinterpret_cast<const unsigned char*>(der.constData()) + der.size()) certificate.reset();
    return certificate;
}
QString name(const X509_NAME* value) {
    if (!value) return {};
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    if (!bio || X509_NAME_print_ex(bio.get(), value, 0, XN_FLAG_RFC2253 & ~ASN1_STRFLGS_ESC_MSB) < 0) return {};
    char* data = nullptr; const long size = BIO_get_mem_data(bio.get(), &data);
    return QString::fromUtf8(data, qsizetype(size));
}
QDateTime time(const ASN1_TIME* value) {
    struct tm parts {};
    if (!value || ASN1_TIME_to_tm(value, &parts) != 1) return {};
    return QDateTime(QDate(parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday), QTime(parts.tm_hour, parts.tm_min, parts.tm_sec), QTimeZone::UTC);
}
// Chain-building failures: the platform verifier (which knows Keychain trust) explains these better.
bool untrustedIssuer(int error) {
    return error == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY || error == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT ||
        error == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT || error == X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN ||
        error == X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE;
}
void addPlatformRoots(X509_STORE* store) {
#if defined(Q_OS_WIN)
    for (const auto* location : {L"ROOT", L"CA"}) {
        HCERTSTORE system = CertOpenSystemStoreW(0, location);
        if (!system) continue;
        for (PCCERT_CONTEXT context = nullptr; (context = CertEnumCertificatesInStore(system, context));) {
            const auto* bytes = context->pbCertEncoded;
            if (X509Pointer certificate{d2i_X509(nullptr, &bytes, long(context->cbCertEncoded))}) X509_STORE_add_cert(store, certificate.get());
        }
        CertCloseStore(system, 0);
    }
#elif !defined(Q_OS_MACOS)
    // The bundled OpenSSL's own directory is the build prefix, so use the distribution's.
    const auto file = qEnvironmentVariable("SSL_CERT_FILE");
    if (!file.isEmpty() && QFile::exists(file)) X509_STORE_load_file(store, QFile::encodeName(file).constData());
    for (const char* path : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem",
                             "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", "/etc/ssl/cert.pem"}) {
        if (QFile::exists(path)) { X509_STORE_load_file(store, path); break; }
    }
    const auto directory = qEnvironmentVariable("SSL_CERT_DIR", "/etc/ssl/certs");
    X509_STORE_load_path(store, QFile::encodeName(directory).constData());
#else
    Q_UNUSED(store);
#endif
}
VncVerification verifyOpenSsl(const QString& host, const QByteArray& leaf, const QList<QByteArray>& chain, const QList<QByteArray>& authorities, int* error) {
    *error = X509_V_OK;
    std::unique_ptr<X509_STORE, StoreFree> store(X509_STORE_new());
    std::unique_ptr<STACK_OF(X509), StackFree> untrusted(sk_X509_new_null());
    std::unique_ptr<X509_STORE_CTX, ContextFree> context(X509_STORE_CTX_new());
    auto target = parse(leaf);
    if (!store || !untrusted || !context || !target) return {false, "The certificate could not be read."};
    for (const auto& authority : authorities) if (auto certificate = parse(authority)) X509_STORE_add_cert(store.get(), certificate.get());
    addPlatformRoots(store.get());
    for (const auto& der : chain) if (auto certificate = parse(der); certificate && sk_X509_push(untrusted.get(), certificate.get())) certificate.release();
    if (X509_STORE_CTX_init(context.get(), store.get(), target.get(), untrusted.get()) != 1) return {false, "The certificate could not be checked."};
    auto* parameters = X509_STORE_CTX_get0_param(context.get());
    X509_VERIFY_PARAM_set_purpose(parameters, X509_PURPOSE_SSL_SERVER);
    const auto hostBytes = host.toUtf8();
    if (QHostAddress(host).isNull() ? X509_VERIFY_PARAM_set1_host(parameters, hostBytes.constData(), size_t(hostBytes.size())) != 1
                                    : X509_VERIFY_PARAM_set1_ip_asc(parameters, hostBytes.constData()) != 1)
        return {false, "The host name could not be checked."};
    if (X509_verify_cert(context.get()) == 1) return {true, {}};
    *error = X509_STORE_CTX_get_error(context.get());
    return {false, QString::fromUtf8(X509_verify_cert_error_string(*error))};
}
#ifdef Q_OS_MACOS
template <typename T> struct CF {
    T value = nullptr;
    ~CF() { if (value) CFRelease(value); }
};
VncVerification verifyKeychain(const QString& host, const QByteArray& leaf, const QList<QByteArray>& chain) {
    CF<CFMutableArrayRef> certificates{CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks)};
    if (!certificates.value) return {false, {}};
    for (const auto& der : QList<QByteArray>{leaf} + chain) {
        CF<CFDataRef> data{CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(der.constData()), CFIndex(der.size()))};
        CF<SecCertificateRef> certificate{data.value ? SecCertificateCreateWithData(nullptr, data.value) : nullptr};
        if (!certificate.value) return {false, "The certificate could not be read."};
        CFArrayAppendValue(certificates.value, certificate.value);
    }
    CF<CFStringRef> hostname{host.toCFString()};
    CF<SecPolicyRef> policy{SecPolicyCreateSSL(true, hostname.value)};
    CF<SecTrustRef> trust;
    if (!policy.value || SecTrustCreateWithCertificates(certificates.value, policy.value, &trust.value) != errSecSuccess) return {false, {}};
    CF<CFErrorRef> error;
    if (SecTrustEvaluateWithError(trust.value, &error.value)) return {true, {}};
    CF<CFStringRef> description{error.value ? CFErrorCopyDescription(error.value) : nullptr};
    return {false, description.value ? QString::fromCFString(description.value) : QString()};
}
#endif
QString endpointKey(const QString& host, quint16 port) { return host.toLower() + '\n' + QString::number(port); }
}

QString VncCertificate::fingerprint() const {
    QStringList bytes; for (const auto byte : sha256) bytes << QString("%1").arg(quint8(byte), 2, 16, QChar('0')).toUpper();
    return bytes.join(':');
}
QString VncCertificate::name() const {
    for (const auto& part : subject.split(',')) if (part.trimmed().startsWith("CN=")) return part.trimmed().mid(3);
    return subject;
}
std::optional<VncCertificate> describeVncCertificate(const QByteArray& der) {
    const auto certificate = parse(der);
    if (!certificate) return std::nullopt;
    VncCertificate result;
    result.der = der; result.sha256 = QCryptographicHash::hash(der, QCryptographicHash::Sha256);
    result.subject = name(X509_get_subject_name(certificate.get())); result.issuer = name(X509_get_issuer_name(certificate.get()));
    result.notBefore = time(X509_get0_notBefore(certificate.get())); result.notAfter = time(X509_get0_notAfter(certificate.get()));
    result.authority = X509_check_ca(certificate.get()) > 0;
    return result;
}
QList<QByteArray> readVncCertificates(const QByteArray& bytes) {
    QList<QByteArray> result;
    if (!bytes.contains("-----BEGIN")) { if (parse(bytes)) result << bytes; return result; }
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(bytes.constData(), int(bytes.size())), BIO_free);
    while (bio) {
        X509Pointer certificate(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
        if (!certificate) break;
        const int size = i2d_X509(certificate.get(), nullptr);
        if (size <= 0) break;
        QByteArray der(size, Qt::Uninitialized); auto* out = reinterpret_cast<unsigned char*>(der.data());
        if (i2d_X509(certificate.get(), &out) == size) result << der;
    }
    return result;
}
VncVerification verifyVncCertificate(const QString& host, const QByteArray& leaf, const QList<QByteArray>& chain, const QList<QByteArray>& authorities) {
    int error = X509_V_OK;
    auto result = verifyOpenSsl(host, leaf, chain, authorities, &error);
#ifdef Q_OS_MACOS
    if (!result.trusted) {
        const auto keychain = verifyKeychain(host, leaf, chain);
        if (keychain.trusted) return keychain;
        if (untrustedIssuer(error) && !keychain.reason.isEmpty()) result.reason = keychain.reason;
    }
#else
    Q_UNUSED(untrustedIssuer);
#endif
    return result;
}

VncTrustStore::VncTrustStore(QString path) : path_(std::move(path)) { if (!path_.isEmpty()) file_.emplace(path_); }
FileResult VncTrustStore::load() {
    if (!file_) return {true, {}, {}};
    QByteArray bytes; bool exists = false;
    file_->reset();
    if (!file_->load(bytes, exists, MaxBytes)) { return {false, "Cannot read the VNC certificate file: unsafe path or oversized file.", {}}; }
    QList<VncCertificate> authorities; QList<VncPinnedServer> servers;
    if (exists) {
        const auto document = QJsonDocument::fromJson(bytes); const auto object = document.object();
        const auto invalid = [&] { return FileResult{false, "The VNC certificate file " + path_ + " is not valid. It was not changed.", {}}; };
        if (!document.isObject() || object.value("version") != 1 || !object.value("authorities").isArray() || !object.value("servers").isArray()) return invalid();
        for (const auto& value : object.value("authorities").toArray()) {
            const auto certificate = describeVncCertificate(QByteArray::fromBase64(value.toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors));
            if (!value.isString() || !certificate) return invalid();
            authorities << *certificate;
        }
        for (const auto& value : object.value("servers").toArray()) {
            const auto entry = value.toObject(); const auto port = entry.value("port").toInt();
            const auto certificate = describeVncCertificate(QByteArray::fromBase64(entry.value("certificate").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors));
            if (!value.isObject() || !entry.value("host").isString() || entry.value("host").toString().isEmpty() || port < 1 || port > 65535 || !certificate) return invalid();
            servers << VncPinnedServer{entry.value("host").toString(), quint16(port), *certificate};
        }
    }
    authorities_ = authorities; servers_ = servers;
    return {true, {}, {}};
}
QList<QByteArray> VncTrustStore::authorityCertificates() const {
    QList<QByteArray> result; for (const auto& authority : authorities_) result << authority.der;
    return result;
}
std::optional<VncCertificate> VncTrustStore::pinned(const QString& host, quint16 port) const {
    for (const auto& server : servers_) if (endpointKey(server.host, server.port) == endpointKey(host, port)) return server.certificate;
    return std::nullopt;
}
FileResult VncTrustStore::save(QList<VncCertificate> authorities, QList<VncPinnedServer> servers) {
    if (authorities.size() + servers.size() > MaxEntries) return {false, "Too many VNC certificates are saved. Remove some first.", {}};
    if (file_) {
        QJsonArray authorityValues, serverValues;
        for (const auto& authority : authorities) authorityValues.append(QString::fromLatin1(authority.der.toBase64()));
        for (const auto& server : servers)
            serverValues.append(QJsonObject{{"host", server.host}, {"port", server.port}, {"certificate", QString::fromLatin1(server.certificate.der.toBase64())}});
        const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"authorities", authorityValues}, {"servers", serverValues}}).toJson();
        const auto result = file_->replace(bytes, MaxBytes, "VNC certificate file");
        if (!result.ok) return result;
        authorities_ = std::move(authorities); servers_ = std::move(servers);
        return result;
    }
    authorities_ = std::move(authorities); servers_ = std::move(servers);
    return {true, {}, {}};
}
FileResult VncTrustStore::importAuthorities(const QByteArray& bytes, int* added) {
    if (added) *added = 0;
    // Re-read first so another window's changes are kept.
    if (const auto loaded = load(); !loaded.ok) return loaded;
    const auto certificates = readVncCertificates(bytes);
    if (certificates.isEmpty()) return {false, "The file does not contain a PEM or DER certificate.", {}};
    auto authorities = authorities_; int count = 0;
    for (const auto& der : certificates) {
        const auto certificate = describeVncCertificate(der);
        if (!certificate) continue;
        if (!certificate->authority) return {false, "\"" + certificate->name() + "\" is not a certificate authority. To trust one server's certificate, connect to it and choose Always trust.", {}};
        if (std::any_of(authorities.begin(), authorities.end(), [&](const VncCertificate& existing) { return existing.sha256 == certificate->sha256; })) continue;
        authorities << *certificate; ++count;
    }
    if (!count) return {true, {}, {}};
    auto result = save(std::move(authorities), servers_);
    if (result.ok && added) *added = count;
    return result;
}
FileResult VncTrustStore::removeAuthority(const QByteArray& sha256) {
    if (const auto loaded = load(); !loaded.ok) return loaded;
    auto authorities = authorities_;
    authorities.removeIf([&](const VncCertificate& authority) { return authority.sha256 == sha256; });
    return authorities.size() == authorities_.size() ? FileResult{true, {}, {}} : save(std::move(authorities), servers_);
}
FileResult VncTrustStore::pin(const QString& host, quint16 port, const QByteArray& der) {
    if (const auto loaded = load(); !loaded.ok) return loaded;
    const auto certificate = describeVncCertificate(der);
    if (host.isEmpty() || !port || !certificate) return {false, "The VNC server certificate could not be saved.", {}};
    auto servers = servers_;
    servers.removeIf([&](const VncPinnedServer& server) { return endpointKey(server.host, server.port) == endpointKey(host, port); });
    servers << VncPinnedServer{host, port, *certificate};
    return save(authorities_, std::move(servers));
}
FileResult VncTrustStore::unpin(const QString& host, quint16 port) {
    if (const auto loaded = load(); !loaded.ok) return loaded;
    auto servers = servers_;
    servers.removeIf([&](const VncPinnedServer& server) { return endpointKey(server.host, server.port) == endpointKey(host, port); });
    return servers.size() == servers_.size() ? FileResult{true, {}, {}} : save(authorities_, std::move(servers));
}
