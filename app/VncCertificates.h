#pragma once
#include "AtomicFile.h"
#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <optional>

// One X.509 certificate (DER), described for review and trust decisions.
struct VncCertificate {
    QByteArray der, sha256;
    QString subject, issuer;
    QDateTime notBefore, notAfter;
    bool authority = false; // basicConstraints CA:TRUE
    QString fingerprint() const; // SHA-256 as AB:CD:…
    QString name() const;        // Common name, or the whole subject
};
std::optional<VncCertificate> describeVncCertificate(const QByteArray& der);
// Every certificate in PEM text (any number of CERTIFICATE blocks) or one DER certificate.
QList<QByteArray> readVncCertificates(const QByteArray& bytes);

struct VncVerification { bool trusted = false; QString reason; };
// Verifies a server certificate (plus the chain the server sent) for host, as a browser
// does: the platform trust store (Keychain trust settings on macOS, the system bundle on
// Linux, the Windows root store) and the imported authorities, with the host name or IP
// address checked against the certificate. Blocking; call it off the GUI thread.
VncVerification verifyVncCertificate(const QString& host, const QByteArray& leaf, const QList<QByteArray>& chain,
                                      const QList<QByteArray>& authorities);

// A server certificate that OpenSSL and the platform could not verify, awaiting the user.
struct VncCertificateReview {
    QString host;
    quint16 port = 0;
    VncCertificate certificate;
    QString reason;
    std::optional<VncCertificate> previous; // set when a different certificate was pinned
};

struct VncPinnedServer { QString host; quint16 port = 0; VncCertificate certificate; };

// Imported certificate authorities and always-trusted server certificates, kept in
// vnc-certificates.json. GUI thread; the file is small and replaced atomically. An empty
// path keeps everything in memory.
class VncTrustStore {
public:
    static constexpr qsizetype MaxBytes = 1 << 20;
    static constexpr int MaxEntries = 256;
    explicit VncTrustStore(QString path);
    FileResult load();
    const QList<VncCertificate>& authorities() const { return authorities_; }
    const QList<VncPinnedServer>& servers() const { return servers_; }
    QList<QByteArray> authorityCertificates() const;
    std::optional<VncCertificate> pinned(const QString& host, quint16 port) const;
    // PEM or DER bytes holding one or more CA certificates; added counts new ones.
    FileResult importAuthorities(const QByteArray& bytes, int* added = nullptr);
    FileResult removeAuthority(const QByteArray& sha256);
    FileResult pin(const QString& host, quint16 port, const QByteArray& der);
    FileResult unpin(const QString& host, quint16 port);
private:
    FileResult save(QList<VncCertificate> authorities, QList<VncPinnedServer> servers);
    QString path_;
    std::optional<AtomicFile> file_;
    QList<VncCertificate> authorities_;
    QList<VncPinnedServer> servers_;
};
