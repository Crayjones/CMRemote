#pragma once
#include "ClipboardTransfer.h"
#include "RdpTarget.h"
#include "DesktopUpdate.h"
#include "RemoteCursor.h"
#include "Secret.h"
#include "VncAuthentication.h"
#include "VncCertificates.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <memory>

struct VncCommand;
// GUI-affine facade over one LibVNCClient connection on its own native thread.
// Supports None, VncAuth and VeNCrypt (Plain, TLSNone/Vnc/Plain, X509None/Vnc/Plain),
// server cursor shapes, text clipboard and client-requested desktop resize.
class VncWorker final : public QObject {
    Q_OBJECT
public:
    // PasswordRequired waits for credentials(); prompt() says what the server asked for.
    // CertificateReview waits for trustCertificate(); certificateReview() describes it.
    enum class Phase { Resolving, Connecting, Handshaking, Connected, Disconnected, Cancelled, Failed, PasswordRequired, CertificateReview };
    Q_ENUM(Phase)
    enum class Prompt { Password, UsernamePassword };
    enum class CertificateTrust { Reject, Once, Always };
    struct Options {
        // DER certificate authorities trusted for the X509 VeNCrypt types, on top of the
        // platform trust store.
        QList<QByteArray> authorities;
        // The server certificate the user chose to always trust for this host and port.
        std::optional<VncCertificate> pinned;
        // Username for the VeNCrypt Plain types, used without asking when a password is
        // also available.
        QString username;
    };
    VncWorker(SessionTarget target, VncAuthentication authentication, quint64 generation, Options options = {}, QObject* parent = nullptr);
    ~VncWorker() override;
    Phase phase() const { return phase_; }
    quint64 generation() const { return generation_; }
    Prompt prompt() const { return prompt_; }
    QString promptUsername() const { return promptUsername_; }
    // Negotiated security, e.g. "VeNCrypt X509Plain"; set once Connected.
    QString security() const { return security_; }
    bool encrypted() const { return encrypted_; }
    // How the server's identity was established, e.g. "certificate verified"; empty
    // when the security type has none (VncAuth, Plain, anonymous TLS). Set once Connected.
    QString identity() const { return identity_; }
    const VncCertificateReview& certificateReview() const { return review_; }
    bool key(quint32 keysym, bool down, quint64 generation, QString* error = nullptr);
    bool pointer(quint8 mask, quint16 x, quint16 y, quint64 generation, QString* error = nullptr);
    // Answers PasswordRequired. An empty password uses the saved one, if any.
    bool credentials(QString username, std::optional<Secret> password, quint64 generation, QString* error = nullptr);
    bool password(Secret password, quint64 generation, QString* error = nullptr) { return credentials({}, std::move(password), generation, error); }
    // Answers CertificateReview. Once and Always both connect; saving an Always pin is the caller's.
    bool trustCertificate(CertificateTrust trust, quint64 generation);
    // Requests a server-side desktop size (ExtendedDesktopSize); ignored until resizeAvailable.
    void resizeDesktop(QSize size);
    // Text clipboard (RFB cut text, Latin-1). Inert until configured with an enabled control.
    void configureClipboard(ClipboardControl control);
    void clipboardText(QString text, ClipboardStamp stamp);
    std::optional<ClipboardDelivery> takeClipboard();
    DesktopUpdate takeUpdate();
    std::optional<RemoteCursor> takeCursor();
    void stop();
    static int activeWorkers();
    static constexpr int StageTimeoutMs = 30000;
    static constexpr size_t InputCapacity = 1024;
signals:
    void frameAvailable(quint64 generation);
    void cursorAvailable(quint64 generation);
    void clipboardAvailable(quint64 generation);
    void resizeAvailable(bool available, quint64 generation);
    void phaseChanged(VncWorker::Phase phase, QString error, quint64 generation);
    // An unencrypted security type was negotiated with a public internet address.
    void securityWarning(QString text, quint64 generation);
    // Native thread has been joined off GUI and all retained deliveries drained.
    void finished(quint64 generation);
public:
    struct State;
private:
    static void drain(const std::shared_ptr<State>& state, const QPointer<VncWorker>& owner);
    void abortLookup();
    bool enqueue(VncCommand command, QString* error);
    std::shared_ptr<State> state_;
    const quint64 generation_;
    Phase phase_ = Phase::Resolving;
    Prompt prompt_ = Prompt::Password;
    QString promptUsername_, security_, identity_, exposed_, host_;
    VncCertificateReview review_;
    bool encrypted_ = false, resizable_ = false;
    ClipboardControl clipboard_;
    std::optional<quint8> pointerMask_; // buttons of the last queued pointer event
    QTimer deadline_;
    int lookup_ = -1;
};
