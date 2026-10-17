#pragma once
#include "remotemanager-core/src/bridge.rs.h"
#include <QWidget>
#include <QPointer>
#include "SshAuthentication.h"
#include "VncAuthentication.h"
#include "SshHostKeyStore.h"
class SshWorker;
class VncWorker;
class VncDesktopView;
class SshHostKeyService;
class QDialog;
#include "RdpTarget.h"
#include "DocumentIdentity.h"
#include "SessionClipboard.h"
class QLabel;
class IdleTimeout;
class DesktopView;
class TerminalView;
class RdpWorker;
struct RdpConfig;
class SessionPane final : public QWidget {
    Q_OBJECT
    Q_PROPERTY(bool clipboardSharingEnabled READ clipboardSharingEnabled WRITE setClipboardSharingEnabled NOTIFY clipboardSharingChanged)
public:
    SessionPane(const rm::SessionInfo& info, QString profile, QWidget* parent = nullptr, DocumentId document = {}, std::shared_ptr<ClipboardEndpoint> clipboard = {});
    bool clipboardSharingEnabled() const { return clipboard_->enabled(); }
    bool clipboardSharingAvailable() const { return target_.protocol == "rdp" && !mock_ && active_ && connected_; }
    void setClipboardSharingEnabled(bool enabled);
    DocumentId document() const { return document_; }
    ProfileReference reference() const { return {document_, profile_}; }
    ~SessionPane() override;
    QString id() const { return id_; }
    QString profile() const { return profile_; }
    bool mock() const { return mock_; }
    SessionTarget target() const { return target_; }
    bool active() const { return active_; }
    bool graphicalDesktop() const { return desktop_ && (target_.protocol == "rdp" || target_.protocol == "vnc"); }
    void event(const QString& kind, const QString& text);
    void startRdp(RdpConfig config);
    SshWorker* startSsh(rust::Box<rm::ssh::Authentication> authentication, SshHostKeyService* trust);
    VncWorker* startVnc(rust::Box<rm::vnc::Authentication> authentication);
    bool vncRetiring() const;
    void afterVncRetirement(std::function<void()> complete);
    bool sshRetiring() const;
    void afterSshRetirement(std::function<void()> complete);
    void shutdown();
    // RDP mouse capture (relative mouse for games); released by the chord or focus loss.
    bool pointerCaptureAvailable() const;
    bool pointerCaptured() const;
    void setPointerCaptured(bool captured);
signals:
    void input(QString id, QString description);
    void resized(QString id, QSize size);
    void state(QString id, QString state);
    void clipboardSharingChanged(bool enabled);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    void refreshClipboardFocus();
    void cancelSshTrust();
    void inspectSshHost(const QString& request, SshHostKey key, quint64 generation);
    void sshTrustReply(const QString& request, SshHostKey key, quint64 generation, SshHostKeyReply reply);
    void decideSshHost(const QString& request, SshHostKey key, quint64 generation, SshHostKeyDecision decision);
    bool currentSsh(const QString& request, quint64 generation) const;
    QPointer<SshWorker> sshWorker_;
    void closeVncPassword();
    QPointer<VncWorker> vncWorker_;
    QPointer<QDialog> vncPassword_;
    VncDesktopView* vncDesktop_ = nullptr;
    bool acceptingVncFrames_ = false;
    QString vncFailure_;
    QPointer<SshHostKeyService> sshTrust_;
    QPointer<QDialog> sshDialog_;
    QString sshHostRequest_, sshTrustRequest_;
    std::shared_ptr<ClipboardEndpoint> terminalClipboard_;
    quint64 connectionGeneration_ = 0;
    SessionClipboard* clipboard_ = nullptr;
    bool connected_ = false;
    QString id_, profile_;
    const DocumentId document_;
    const SessionTarget target_;
    bool mock_, active_ = true;
    QLabel* status_;
    QLabel* captureHint_ = nullptr;
    IdleTimeout* idle_ = nullptr;
    DesktopView* desktop_ = nullptr;
    TerminalView* terminal_ = nullptr;
#ifdef RM_WITH_RDP
    RdpWorker* worker_ = nullptr;
    bool resizeReady_ = false, acceptingFrames_ = false;
#endif
};
