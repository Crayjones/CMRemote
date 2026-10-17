#include "SessionPane.h"
#include "Views.h"
#include "TerminalScreen.h"
#include "SshWorker.h"
#include "VncWorker.h"
#include "VncCredentialsDialog.h"
#include "VncDesktopView.h"
#include "SshHostKeyService.h"
#include "SshHostKeyDialog.h"
#include <QApplication>
#include <QEvent>
#include "RdpWorker.h"
#include "Credentials.h"
#include "IdleTimeout.h"
#include <QGuiApplication>
#include <QLabel>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
#include <utility>
static QString qstr(const rust::String& s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }
SessionPane::SessionPane(const rm::SessionInfo& info, QString profile, QWidget* parent, DocumentId document, std::shared_ptr<ClipboardEndpoint> clipboard)
    : QWidget(parent), id_(qstr(info.id)), profile_(std::move(profile)), document_(document), target_(sessionTarget(info)), mock_(info.mock) {
    terminalClipboard_ = clipboard ? std::move(clipboard) : systemClipboardEndpoint();
    clipboard_ = new SessionClipboard(terminalClipboard_, this);
    installEventFilter(this);
    connect(qApp, &QApplication::focusChanged, this, [this] { refreshClipboardFocus(); });
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this] { refreshClipboardFocus(); });
    setContextMenuPolicy(Qt::PreventContextMenu);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    status_ = new QLabel(info.mock ? "MOCK — no network connection" : target_.protocol.toUpper() + " — connecting", this);
    status_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    status_->setObjectName("sessionStatus"); layout->addWidget(status_);
    idle_ = new IdleTimeout(this); idle_->setObjectName("rdpIdleTimeout");
    captureHint_ = new QLabel(this); captureHint_->setObjectName("sessionCaptureHint");
    captureHint_->setStyleSheet("QLabel { background: #1f4e8c; color: white; padding: 3px 6px; }");
    captureHint_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed); captureHint_->hide(); layout->addWidget(captureHint_);
    if (info.graphical) {
        if (target_.protocol == "vnc") {
            vncDesktop_ = new VncDesktopView(info.mock, this); desktop_ = vncDesktop_;
            vncDesktop_->setInputEnabled(info.mock && !target_.viewOnly);
            if (info.mock) connect(vncDesktop_, &VncDesktopView::inputError, this, [this](const QString& error) {
                if (active_) { status_->setText(error); status_->show(); }
            });
        } else desktop_ = new DesktopView(info.mock, this);
        desktop_->installEventFilter(this); layout->addWidget(desktop_, 1);
        connect(desktop_, &DesktopView::mockInput, this, [this](const QString& s) { if (active_) emit input(id_, s); });
        connect(desktop_, &DesktopView::viewportChanged, this, [this](QSize s) { if (active_ && mock_) emit resized(id_, s); });
    } else {
        terminal_ = new TerminalView(this); layout->addWidget(terminal_, 1);
        connect(terminal_, &TerminalView::mockInput, this, [this](const QString& s) { if (active_) emit input(id_, s); });
        connect(terminal_, &TerminalView::viewportChanged, this, [this](QSize s) { if (active_) emit resized(id_, s); });
    }
}
SessionPane::~SessionPane() { shutdown(); }
void SessionPane::refreshClipboardFocus() {
    clipboard_->setFocused(clipboardSharingAvailable() && desktop_ && desktop_->hasFocus() && desktop_->isVisible() &&
        desktop_->isActiveWindow() && QGuiApplication::applicationState() == Qt::ApplicationActive);
}
bool SessionPane::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Show || event->type() == QEvent::Hide || event->type() == QEvent::WindowActivate || event->type() == QEvent::WindowDeactivate)
        QTimer::singleShot(0, this, [this] { refreshClipboardFocus(); });
    return QWidget::eventFilter(watched, event);
}
void SessionPane::setClipboardSharingEnabled(bool enabled) {
    if (enabled && !clipboardSharingAvailable()) return;
    const bool previous = clipboard_->enabled();
    refreshClipboardFocus(); clipboard_->setEnabled(enabled);
    if (previous != clipboard_->enabled()) emit clipboardSharingChanged(clipboard_->enabled());
}
QString SessionPane::statusDetail() const { return status_ ? status_->text() : QString(); }
void SessionPane::event(const QString& kind, const QString& text) {
    if (!active_) return;
    status_->setText("MOCK — " + text);
    if (desktop_) desktop_->report(text);
    if (terminal_) terminal_->report(text);
    if (kind == "state") emit state(id_, text);
}
void SessionPane::shutdown() {
    if (!active_) return;
    // Release while this worker/generation is still current, then disable input.
    if (vncDesktop_) { vncDesktop_->releaseInput(); vncDesktop_->setInputEnabled(false); }
    setClipboardSharingEnabled(false); clipboard_->reset(); connected_ = false;
    active_ = false; ++connectionGeneration_;
    acceptingVncFrames_ = false;
    closeVncPassword();
    if (vncWorker_) vncWorker_->stop();
    cancelSshTrust();
    if (sshWorker_) sshWorker_->stop();
    if (terminal_) terminal_->setInputEnabled(false);
    if (desktop_) desktop_->resetRemoteCursor();
#ifdef RM_WITH_RDP
    acceptingFrames_ = false;
    if (idle_) idle_->stop();
    if (desktop_) { desktop_->setPointerCaptured(false); desktop_->setPointerCaptureAllowed(false); }
    if (worker_) {
        worker_->stop();
        // Credential prompts answer "cancel" to the stopping worker.
        for (auto* dialog : findChildren<QDialog*>()) dialog->reject();
        worker_->wait(); delete worker_; worker_ = nullptr;
    }
#endif
    status_->setText("Disconnected — use Reconnect or close this tab"); status_->show();
}
bool SessionPane::pointerCaptureAvailable() const { return desktop_ && target_.protocol == "rdp" && !mock_ && active_ && connected_; }
bool SessionPane::pointerCaptured() const { return desktop_ && desktop_->pointerCaptured(); }
void SessionPane::setPointerCaptured(bool captured) {
    if (!desktop_ || (captured && !pointerCaptureAvailable())) return;
    desktop_->setPointerCaptured(captured);
}
void SessionPane::startRdp(RdpConfig config) {
#ifdef RM_WITH_RDP
    if (worker_) shutdown();
    if (!desktop_) return;
    setClipboardSharingEnabled(false); clipboard_->reset(); connected_ = false;
    const bool clipboardConsent = config.clipboard;
    const auto generation = ++connectionGeneration_;
    active_ = true; acceptingFrames_ = true; resizeReady_ = false;
    desktop_->resetRemoteCursor(); desktop_->setFrame({});
    desktop_->setPerformanceTrace(config.performance);
    layout()->activate(); config.panelSize = desktop_->size();
    const auto options = config.options;
    worker_ = new RdpWorker(std::move(config), this);
    QPointer<RdpWorker> worker = worker_;
    auto current = [this, worker, generation] {
        return active_ && worker && worker_ == worker && generation == connectionGeneration_;
    };
    auto* resizeTimer = new QTimer(worker_); resizeTimer->setSingleShot(true); resizeTimer->setInterval(200);
    connect(worker_, &RdpWorker::frameAvailable, this, [this, worker, current, resizeTimer] {
        if (!current() || !acceptingFrames_ || !worker->isRunning()) return;
        const auto update = worker->takeUpdate();
        if (update.patches.empty()) return;
        const bool first = desktop_->frame().isNull();
        if (!desktop_->applyUpdate(update)) {
            acceptingFrames_ = false; desktop_->resetRemoteCursor(); setClipboardSharingEnabled(false); connected_ = false; worker->stop();
            status_->setText("RDP desktop update rejected; closing session"); status_->show(); return;
        }
        if (first && resizeReady_) resizeTimer->start();
    }, Qt::QueuedConnection);
    connect(worker_, &RdpWorker::cursorAvailable, this, [this, worker, current] {
        if (!current() || !acceptingFrames_ || !worker->isRunning()) return;
        if (auto cursor = worker->takeCursor()) desktop_->setRemoteCursor(std::move(*cursor));
    }, Qt::QueuedConnection);
    connect(worker_, &RdpWorker::stateChanged, this, [this, current, clipboardConsent](const QString& s, const QString& detail) {
        if (!current()) return;
        if (s == "connected") idle_->start(); else idle_->stop();
        if (s == "disconnected" || s == "failed") {
            desktop_->setPointerCaptured(false);
            acceptingFrames_ = false; desktop_->resetRemoteCursor(); setClipboardSharingEnabled(false); clipboard_->reset(); connected_ = false;
        } else if (s == "connected") { connected_ = true; refreshClipboardFocus(); setClipboardSharingEnabled(clipboardConsent); }
        status_->setText(detail); status_->setVisible(s != "connected"); emit state(id_, s);
    }, Qt::QueuedConnection);
    connect(worker_, &RdpWorker::credentialsRequired, this, [this, worker, current](bool gateway, const QString& endpoint, const QString& username, const QString& domain) {
        if (!current()) { if (worker) worker->provideCredentials(std::nullopt); return; }
        PromptCredentialProvider provider;
        auto* dialog = provider.request(this, {(gateway ? "RDP gateway " : "RDP server ") + endpoint, username, true, domain, false},
            [worker](std::optional<TransientCredentials> credentials) { if (worker) worker->provideCredentials(std::move(credentials)); });
        if (dialog) dialog->setObjectName(gateway ? "rdpGatewayCredentials" : "rdpServerCredentials");
    }, Qt::QueuedConnection);
    connect(desktop_, &DesktopView::viewportChanged, resizeTimer, [resizeTimer] { resizeTimer->start(); });
    // Channel capabilities can arrive before the server finishes its initial desktop update.
    connect(resizeTimer, &QTimer::timeout, this, [this, current] {
        if (current() && resizeReady_ && !desktop_->frame().isNull()) worker_->resizeDesktop(desktop_->size());
    });
    connect(worker_, &RdpWorker::resizeAvailable, this, [this, current, resizeTimer](bool ready) {
        if (!current()) return;
        resizeReady_ = ready;
        if (ready && !desktop_->frame().isNull()) resizeTimer->start();
    }, Qt::QueuedConnection);
    // Idle = no local keyboard, text, pointer or wheel input to this session (V104).
    idle_->configure(options.idleMinutes);
    idle_->disconnect(); disconnect(desktop_, nullptr, idle_, nullptr);
    connect(idle_, &IdleTimeout::expired, this, [this, current] {
        if (!current()) return;
        const auto minutes = idle_->minutes();
        emit state(id_, "disconnected"); shutdown();
        status_->setText(QString("Disconnected after %1 minute%2 without input (Minutes to Idle). Use Reconnect or close this tab.")
            .arg(minutes).arg(minutes == 1 ? "" : "s"));
    });
    connect(desktop_, &DesktopView::keyInput, idle_, &IdleTimeout::activity);
    connect(desktop_, &DesktopView::textInput, idle_, &IdleTimeout::activity);
    connect(desktop_, &DesktopView::pointerInput, idle_, &IdleTimeout::activity);
    desktop_->setRelativePointerSupported(false); desktop_->setPointerCaptureAllowed(true);
    disconnect(desktop_, &DesktopView::pointerCaptureChanged, this, nullptr);
    connect(desktop_, &DesktopView::pointerCaptureChanged, this, [this](bool captured) {
        captureHint_->setText(QString("Mouse captured — press %1 to release").arg(DesktopView::captureChordText()));
        captureHint_->setVisible(captured);
    });
    connect(worker_, &RdpWorker::relativePointerAvailable, this, [this, current](bool available) {
        if (current()) desktop_->setRelativePointerSupported(available);
    }, Qt::QueuedConnection);
    connect(worker_, &RdpWorker::remotePointerMoved, this, [this, current](QPoint position) {
        if (current()) desktop_->setRemotePointerPosition(position);
    }, Qt::QueuedConnection);
    connect(desktop_, &DesktopView::relativePointerInput, worker_, &RdpWorker::relativePointer);
    connect(desktop_, &DesktopView::keyInput, worker_, &RdpWorker::key);
    connect(desktop_, &DesktopView::textInput, worker_, &RdpWorker::text);
    connect(desktop_, &DesktopView::pointerInput, worker_, &RdpWorker::pointer);
    // Negotiation is inert until this session has explicit consent and a current route.
    worker_->configureClipboard({});
    connect(clipboard_, &SessionClipboard::configure, worker_, &RdpWorker::configureClipboard);
    connect(clipboard_, &SessionClipboard::localText, worker_, &RdpWorker::clipboardText);
    connect(worker_, &RdpWorker::clipboardAvailable, this, [this, worker, current] {
        if (!current()) return;
        auto delivery = worker->takeClipboard();
        if (connected_ && clipboard_->enabled() && delivery) clipboard_->receive(std::move(*delivery));
    }, Qt::QueuedConnection);
    worker_->start(); desktop_->setFocus();
#else
    Q_UNUSED(config);
#endif
}
void SessionPane::closeVncPassword() { auto dialog = vncPassword_; vncPassword_.clear(); if (dialog) dialog->reject(); }
bool SessionPane::vncRetiring() const { return bool(vncWorker_); }
void SessionPane::afterVncRetirement(std::function<void()> complete) {
    if (!vncWorker_) { QTimer::singleShot(0, this, std::move(complete)); return; }
    connect(vncWorker_, &VncWorker::finished, this, [complete = std::move(complete)](quint64) { complete(); }, Qt::SingleShotConnection);
}
VncWorker* SessionPane::startVnc(rust::Box<rm::vnc::Authentication> authentication) {
    if (!vncDesktop_ || mock_ || vncWorker_ || target_.protocol != "vnc") return nullptr;
    setClipboardSharingEnabled(false); clipboard_->reset(); connected_ = false;
    vncDesktop_->setInputEnabled(false); desktop_->resetRemoteCursor(); desktop_->setFrame({});
    active_ = true; acceptingVncFrames_ = true; vncFailure_.clear();
    const auto generation = ++connectionGeneration_;
    auto* worker = new VncWorker(target_, std::move(authentication), generation);
    vncWorker_ = worker;
    connect(worker, &VncWorker::finished, worker, &QObject::deleteLater);
    connect(worker, &VncWorker::finished, this, [this, worker](quint64) { if (vncWorker_ == worker) vncWorker_.clear(); });
    connect(worker, &VncWorker::securityWarning, this, [this, worker](const QString& text) { if (vncWorker_ == worker) emit notice(text); });
    const auto current = [this, worker, generation] { return active_ && vncWorker_ == worker && connectionGeneration_ == generation; };
    const auto failInput = [this, worker](const QString& error) {
        connected_ = false; acceptingVncFrames_ = false; vncDesktop_->setInputEnabled(false);
        desktop_->resetRemoteCursor(); worker->stop();
        vncFailure_ = error.isEmpty() ? "VNC input could not be delivered; closing session" : error;
        status_->setText(vncFailure_); status_->show();
        emit state(id_, "failed");
    };
    connect(vncDesktop_, &VncDesktopView::inputError, worker, [current, failInput](const QString& error) {
        if (current()) failInput(error);
    });
    connect(vncDesktop_, &VncDesktopView::vncKeyInput, worker, [this, worker, current, generation, failInput](quint32 keysym, bool down) {
        if (!current() || !connected_ || target_.viewOnly) return;
        QString error; if (!worker->key(keysym, down, generation, &error)) failInput(error);
    });
    connect(vncDesktop_, &VncDesktopView::vncPointerInput, worker, [this, worker, current, generation, failInput](quint8 mask, quint16 x, quint16 y) {
        if (!current() || !connected_ || target_.viewOnly) return;
        QString error; if (!worker->pointer(mask, x, y, generation, &error)) failInput(error);
    });
    connect(worker, &VncWorker::frameAvailable, this, [this, worker, current, generation](quint64 deliveryGeneration) {
        if (deliveryGeneration != generation || !current() || !acceptingVncFrames_) return;
        const auto update = worker->takeUpdate();
        if (update.patches.empty()) return;
        if (!desktop_->applyUpdate(update)) {
            acceptingVncFrames_ = false; connected_ = false; vncDesktop_->setInputEnabled(false);
            desktop_->resetRemoteCursor(); worker->stop();
            vncFailure_ = "VNC desktop update rejected; closing session";
            status_->setText(vncFailure_); status_->show(); emit state(id_, "failed");
        }
    });
    connect(worker, &VncWorker::cursorAvailable, this, [this, worker, current, generation](quint64 deliveryGeneration) {
        if (deliveryGeneration != generation || !current() || !acceptingVncFrames_) return;
        if (auto cursor = worker->takeCursor()) desktop_->setRemoteCursor(std::move(*cursor));
    });
    connect(worker, &VncWorker::phaseChanged, this, [this, worker, current, generation](VncWorker::Phase phase, const QString& error, quint64 deliveryGeneration) {
        if (deliveryGeneration != generation || !current()) return;
        if (phase == VncWorker::Phase::PasswordRequired) {
            status_->setText("VNC — password required (UNENCRYPTED)"); status_->show(); emit state(id_, "connecting");
            if (vncPassword_) return;
            auto* dialog = new VncCredentialsDialog([this, worker, current, generation](std::optional<Secret> password) {
                if (!current() || !vncPassword_) return;
                vncPassword_.clear();
                // Cancel ends this connection attempt; the tab stays for Reconnect.
                if (!password) { shutdown(); emit state(id_, "disconnected"); return; }
                QString failure;
                if (!worker->password(std::move(*password), generation, &failure)) {
                    status_->setText(failure.isEmpty() ? "VNC password could not be delivered" : failure); status_->show();
                }
            }, this);
            // show(), not open(): open() forces window modality, which is a title-less sheet on macOS.
            vncPassword_ = dialog; dialog->show(); dialog->raise(); dialog->activateWindow(); return;
        }
        closeVncPassword();
        connected_ = phase == VncWorker::Phase::Connected && vncFailure_.isEmpty();
        vncDesktop_->setInputEnabled(connected_ && !target_.viewOnly);
        QString stateName = "connecting", detail = "VNC — connecting (UNENCRYPTED)";
        if (connected_) { stateName = "connected"; detail = target_.viewOnly ? "VNC — connected, view-only (UNENCRYPTED)" : "VNC — connected (UNENCRYPTED)"; }
        else if (phase == VncWorker::Phase::Resolving) detail = "VNC — resolving host (UNENCRYPTED)";
        else if (phase == VncWorker::Phase::Handshaking) detail = "VNC — authenticating (UNENCRYPTED)";
        else if (phase == VncWorker::Phase::Failed || phase == VncWorker::Phase::Disconnected || phase == VncWorker::Phase::Cancelled) {
            acceptingVncFrames_ = false; desktop_->resetRemoteCursor();
            stateName = phase == VncWorker::Phase::Failed ? "failed" : "disconnected";
            detail = error.isEmpty() ? "VNC — disconnected; use Reconnect or close this tab" : error;
        }
        if (!vncFailure_.isEmpty()) detail = vncFailure_;
        // Like RDP, a connected desktop uses the whole pane; the unencrypted
        // notice is shown in the credentials prompt before every connection.
        status_->setText(detail); status_->setVisible(!connected_); emit state(id_, stateName);
    });
    desktop_->setFocus(); return worker;
}
bool SessionPane::sshRetiring() const { return bool(sshWorker_); }
void SessionPane::afterSshRetirement(std::function<void()> complete) {
    if (!sshWorker_) { QTimer::singleShot(0, this, std::move(complete)); return; }
    connect(sshWorker_, &SshWorker::finished, this, [complete = std::move(complete)](quint64) { complete(); }, Qt::SingleShotConnection);
}
SshWorker* SessionPane::startSsh(rust::Box<rm::ssh::Authentication> authentication, SshHostKeyService* trust) {
    if (!terminal_ || mock_ || sshWorker_ || !trust) return nullptr;
    terminal_->initializeTerminal(terminalClipboard_); terminal_->setInputEnabled(false);
    sshTrust_ = trust; connected_ = false; active_ = true;
    const auto generation = ++connectionGeneration_;
    auto* worker = new SshWorker(target_, std::move(authentication), generation);
    sshWorker_ = worker;
    connect(worker, &SshWorker::finished, worker, &QObject::deleteLater);
    connect(worker, &SshWorker::finished, this, [this, worker](quint64) { if (sshWorker_ == worker) sshWorker_.clear(); });
    const auto current = [this, worker, generation] { return active_ && sshWorker_ == worker && connectionGeneration_ == generation; };
    terminal_->setInputSink([this, current, generation](const QByteArray& bytes) { return current() && connected_ && sshWorker_->input(bytes, generation); });
    connect(terminal_, &TerminalView::cellsChanged, worker, [worker, generation](quint16 rows, quint16 columns) { worker->resize(rows, columns, generation); });
    connect(terminal_, &TerminalView::inputError, this, [this](const QString& error) { status_->setText(error); status_->show(); });
    connect(worker, &SshWorker::output, this, [this, current](const QByteArray& bytes, quint64) {
        if (!current()) return;
        if (!terminal_->append(bytes)) { status_->setText("SSH terminal output exceeded supported limits."); status_->show(); shutdown(); emit state(id_, "failed"); }
    });
    connect(worker, &SshWorker::hostKey, this, [this, current](const QString& request, SshHostKey key, quint64 generation) {
        // Server keys are accepted without prompting or saving trust.
        Q_UNUSED(key); if (current()) sshWorker_->answerHost(request, generation, true);
    });
    connect(worker, &SshWorker::phaseChanged, this, [this, current, worker, generation](SshWorker::Phase phase, const QString& error, quint64) {
        if (!current()) return;
        connected_ = phase == SshWorker::Phase::Connected; terminal_->setInputEnabled(connected_);
        QString stateName = "connecting", detail = "SSH — connecting";
        if (connected_) {
            stateName = "connected"; detail = "SSH — connected";
            const auto size = terminal_->screen()->cells(); worker->resize(quint16(size.height()), quint16(size.width()), generation);
        } else if (phase == SshWorker::Phase::WaitingTrust) detail = "SSH — verify server key";
        else if (phase == SshWorker::Phase::Authenticating) detail = "SSH — authenticating";
        else if (phase == SshWorker::Phase::Failed || phase == SshWorker::Phase::Disconnected || phase == SshWorker::Phase::Cancelled) {
            cancelSshTrust(); stateName = phase == SshWorker::Phase::Failed ? "failed" : "disconnected";
            detail = error.isEmpty() ? "SSH — disconnected; use Reconnect or close this tab" : error;
        }
        status_->setText(detail); status_->setVisible(!connected_); emit state(id_, stateName);
    });
    terminal_->setFocus(); return worker;
}
bool SessionPane::currentSsh(const QString& request, quint64 generation) const {
    return active_ && sshWorker_ && generation == connectionGeneration_ && request == sshHostRequest_ &&
        sshWorker_->phase() == SshWorker::Phase::WaitingTrust;
}
void SessionPane::cancelSshTrust() {
    sshHostRequest_.clear();
    const auto request = std::exchange(sshTrustRequest_, {});
    if (sshTrust_ && !request.isEmpty()) sshTrust_->cancel(request);
    auto dialog = sshDialog_; sshDialog_.clear(); if (dialog) dialog->reject();
}
void SessionPane::inspectSshHost(const QString& request, SshHostKey key, quint64 generation) {
    if (!currentSsh(request, generation) || !sshTrust_) return;
    const auto owner = QPointer<SessionPane>(this);
    sshTrustRequest_ = sshTrust_->check(key, generation, [owner, request, key, generation](SshHostKeyReply reply) {
        if (owner && owner->currentSsh(request, generation)) owner->sshTrustReply(request, key, generation, std::move(reply));
    });
}
void SessionPane::decideSshHost(const QString& request, SshHostKey key, quint64 generation, SshHostKeyDecision decision) {
    if (!currentSsh(request, generation) || !sshTrust_) return;
    sshDialog_.clear();
    if (decision == SshHostKeyDecision::Cancel) { sshWorker_->answerHost(request, generation, false); cancelSshTrust(); return; }
    const auto owner = QPointer<SessionPane>(this);
    sshTrust_->decide(sshTrustRequest_, generation, decision, [owner, request, key, generation](SshHostKeyReply reply) {
        if (owner && owner->currentSsh(request, generation)) owner->sshTrustReply(request, key, generation, std::move(reply));
    });
}
void SessionPane::sshTrustReply(const QString& request, SshHostKey key, quint64 generation, SshHostKeyReply reply) {
    if (!currentSsh(request, generation) || reply.generation != generation) return;
    if (reply.allowed) { sshWorker_->answerHost(request, generation, true); cancelSshTrust(); return; }
    if (reply.conflict && !sshTrustRequest_.isEmpty()) { sshTrustRequest_.clear(); inspectSshHost(request, key, generation); return; }
    if (!reply.result.ok || reply.canceled || !reply.inspection) {
        sshWorker_->answerHost(request, generation, false); cancelSshTrust();
        status_->setText(reply.result.error.isEmpty() ? "SSH server key could not be approved." : reply.result.error); status_->show(); return;
    }
    const auto owner = QPointer<SessionPane>(this);
    auto complete = [owner, request, key, generation](SshHostKeyDecision decision) {
        if (owner) owner->decideSshHost(request, key, generation, decision);
    };
    auto* dialog = reply.committed && !reply.result.warning.isEmpty() ? new SshHostKeyDialog(reply, complete, this) :
        new SshHostKeyDialog(*reply.inspection, complete, this);
    sshDialog_ = dialog; dialog->open();
}
