#include "VncWorker.h"
#include "VncGraphics.h"
#include "VncSocket.h"
#include <QHostAddress>
#include <QHostInfo>
#include <QLoggingCategory>
#include <QSet>
#include <QThread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <rfb/rfbclient.h>

Q_LOGGING_CATEGORY(vncLog, "rm.vnc", QtWarningMsg)

struct VncCommand {
    enum class Kind { Key, Pointer, Clipboard, Resize } kind;
    quint32 keysym = 0; bool down = false;
    quint8 mask = 0; quint16 x = 0, y = 0;
    bool motion = false; // the same buttons as the pointer event before it
    QByteArray text;
    QSize size;
};

namespace {
std::atomic<int> liveWorkers{0};
// Loopback, private (RFC 1918 / ULA), link-local and carrier-grade NAT ranges.
bool localNetwork(const QHostAddress& address) {
    if (address.isLoopback() || address.isLinkLocal() || address.isSiteLocal() || address.isUniqueLocalUnicast() || address.isPrivateUse()) return true;
    bool ok = false; const auto v4 = address.toIPv4Address(&ok);
    return ok && (v4 >> 22) == ((100u << 2) | 1u); // 100.64.0.0/10 (Tailscale and other CGNAT overlays)
}
bool terminal(VncWorker::Phase phase) {
    return phase == VncWorker::Phase::Disconnected || phase == VncWorker::Phase::Cancelled || phase == VncWorker::Phase::Failed;
}

// LibVNCClient logs through two process-wide function pointers. Keep the last lines of
// each transport thread so failures can say why; lines never contain credentials.
thread_local std::deque<std::string> recentLog;
void logLine(const char* format, ...) {
    char buffer[512];
    va_list arguments; va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    std::string line(buffer);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.empty()) return;
    qCDebug(vncLog, "%s", line.c_str());
    recentLog.push_back(std::move(line));
    if (recentLog.size() > 24) recentLog.pop_front();
}
const std::string* loggedLine(const char* prefix) {
    for (auto it = recentLog.rbegin(); it != recentLog.rend(); ++it)
        if (it->rfind(prefix, 0) == 0) return &*it;
    return nullptr;
}
char* duplicate(const QByteArray& bytes) {
    auto* copy = static_cast<char*>(std::malloc(size_t(bytes.size()) + 1));
    if (!copy) return nullptr;
    std::memcpy(copy, bytes.constData(), size_t(bytes.size())); copy[bytes.size()] = '\0';
    return copy;
}
char* duplicate(const Secret& secret) {
    auto* copy = static_cast<char*>(std::malloc(secret.size() + 1));
    if (!copy) return nullptr;
    std::memcpy(copy, secret.data(), secret.size()); copy[secret.size()] = '\0';
    return copy;
}
QString securityName(uint32_t scheme, uint32_t sub) {
    switch (sub) {
    case rfbVeNCryptPlain: return "VeNCrypt Plain";
    case rfbVeNCryptTLSNone: return "VeNCrypt TLSNone";
    case rfbVeNCryptTLSVNC: return "VeNCrypt TLSVnc";
    case rfbVeNCryptTLSPlain: return "VeNCrypt TLSPlain";
    case rfbVeNCryptX509None: return "VeNCrypt X509None";
    case rfbVeNCryptX509VNC: return "VeNCrypt X509Vnc";
    case rfbVeNCryptX509Plain: return "VeNCrypt X509Plain";
    default: break;
    }
    const auto plain = [](uint32_t type) { return type == rfbVncAuth ? QString("VncAuth") : type == rfbNoAuth ? QString("None") : QString::number(type); };
    if (scheme == rfbVeNCrypt) return "VeNCrypt " + plain(sub);
    if (scheme == rfbTLS) return "Anonymous TLS " + plain(sub);
    return plain(scheme);
}
// Classic RFB cut text is ISO 8859-1 with LF line ends.
QByteArray latin1CutText(const QString& text) {
    QString normalized = text; normalized.replace("\r\n", "\n"); normalized.replace('\r', '\n');
    QByteArray bytes; bytes.reserve(normalized.size());
    for (const auto ch : normalized) bytes.append(ch.unicode() <= 0xff ? char(ch.unicode()) : '?');
    return bytes;
}
}

struct VncWorker::State {
    // Immutable after construction.
    QString host;
    quint16 port = 0;
    bool viewOnly = false;
    QList<QByteArray> authorities;
    std::optional<VncCertificate> pinned;
    quint64 generation = 0;
    VncWake wake;
    std::atomic<bool> cancelled{false}, notifyPending{false};
    // Guarded by mutex; the transport thread blocks on changed.
    std::mutex mutex;
    std::condition_variable changed;
    Phase phase = Phase::Resolving;
    QString error, abortReason, security, identity;
    bool encrypted = false, resizable = false;
    Prompt prompt = Prompt::Password;
    std::optional<QStringList> addresses;
    std::deque<VncCommand> commands;
    std::optional<Secret> password;
    QString username;
    bool waitingCredentials = false, credentialsAnswered = false, credentialsCancelled = false;
    std::optional<Secret> answeredPassword;
    VncCertificateReview review;
    bool waitingCertificate = false, certificateAnswered = false;
    CertificateTrust certificateAnswer = CertificateTrust::Reject;
    VncSocketHandle socket = invalidVncSocket();
    std::optional<QString> remoteClipboard;
    std::atomic<quint32> geometry{0};
    // Transport thread only.
    bool credentialsUsed = false, plainRequested = false, graphicsFailed = false;
    // The decision for the server certificate, made once per connection.
    std::optional<bool> certificateAccepted;
    bool certificateRejected = false;
    QString certificateIdentity;
    VncGraphics graphics;
    // GUI thread only.
    std::function<void()> notify;
    QObject* delivery = nullptr;
    QThread* worker = nullptr;
    bool stopping = false, joined = false, retired = false;
    bool framePosted = false, cursorPosted = false, clipboardPosted = false;

    void setPhase(Phase value) {
        { std::lock_guard<std::mutex> lock(mutex); if (!terminal(phase)) phase = value; }
        notify();
    }
    void abort(QString reason) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (abortReason.isEmpty() && !cancelled.load()) abortReason = std::move(reason);
            cancelled = true; shutdownVncSocket(socket);
        }
        changed.notify_all(); wake.signal();
    }
    // Transport thread: saved credentials, or wait (unbounded, cancellable) for the user.
    std::optional<std::pair<QString, Secret>> credentials(Prompt kind) {
        std::unique_lock<std::mutex> lock(mutex);
        if (password && (kind == Prompt::Password || !username.isEmpty())) return std::make_pair(username, password->clone());
        prompt = kind; phase = Phase::PasswordRequired;
        waitingCredentials = true; credentialsAnswered = credentialsCancelled = false; answeredPassword.reset();
        lock.unlock(); notify(); lock.lock();
        changed.wait(lock, [this] { return cancelled.load() || credentialsAnswered || credentialsCancelled; });
        waitingCredentials = false;
        if (cancelled.load() || credentialsCancelled) return std::nullopt;
        if (!terminal(phase)) phase = Phase::Handshaking;
        auto answer = std::move(answeredPassword); answeredPassword.reset();
        if (!answer && !password) return std::nullopt;
        auto result = std::make_pair(username, answer ? std::move(*answer) : password->clone());
        lock.unlock(); notify();
        return result;
    }
    // Transport thread, inside the TLS handshake: wait (unbounded, cancellable) for the user.
    std::optional<CertificateTrust> reviewCertificate(VncCertificateReview value) {
        std::unique_lock<std::mutex> lock(mutex);
        review = std::move(value); phase = Phase::CertificateReview;
        waitingCertificate = true; certificateAnswered = false;
        lock.unlock(); notify(); lock.lock();
        changed.wait(lock, [this] { return cancelled.load() || certificateAnswered; });
        waitingCertificate = false;
        if (cancelled.load()) return std::nullopt;
        if (!terminal(phase)) phase = Phase::Handshaking;
        const auto answer = certificateAnswer;
        lock.unlock(); notify();
        return answer;
    }
};

namespace {
char ClientTag;
VncWorker::State* stateOf(rfbClient* client) { return static_cast<VncWorker::State*>(rfbClientGetClientData(client, &ClientTag)); }

rfbBool mallocFramebuffer(rfbClient* client) {
    auto* state = stateOf(client);
    client->frameBuffer = state->graphics.geometry(client->width, client->height);
    if (!client->frameBuffer) { state->graphicsFailed = true; return FALSE; }
    state->geometry.store((quint32(client->width) << 16) | quint32(client->height));
    return TRUE;
}
void gotUpdate(rfbClient* client, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    auto* state = stateOf(client);
    if (!state->graphics.damage(x, y, w, h)) state->graphicsFailed = true;
}
void finishedUpdate(rfbClient* client) {
    auto* state = stateOf(client);
    if (!state->graphics.commit()) { state->graphicsFailed = true; return; }
    if (state->graphics.hasUpdate()) state->notify();
}
void gotCursor(rfbClient* client, int xhot, int yhot, int width, int height, int bytesPerPixel) {
    auto* state = stateOf(client);
    bool ok;
    if (width <= 0 || height <= 0 || !client->rcSource || !client->rcMask) ok = state->graphics.cursor(0, 0, 0, 0, nullptr, 0);
    else if (bytesPerPixel != 4 || width > 384 || height > 384) ok = false;
    else {
        // Cursor pixels use the session pixel format (B,G,R,X); the mask is one byte per pixel.
        std::vector<uchar> bgra(size_t(width) * size_t(height) * 4);
        for (size_t i = 0, n = size_t(width) * size_t(height); i < n; ++i) {
            std::memcpy(&bgra[i * 4], client->rcSource + i * 4, 3);
            bgra[i * 4 + 3] = client->rcMask[i] ? 0xff : 0x00;
        }
        ok = state->graphics.cursor(quint16(qBound(0, xhot, width - 1)), quint16(qBound(0, yhot, height - 1)),
                                    quint16(width), quint16(height), bgra.data(), bgra.size());
    }
    if (!ok) state->graphicsFailed = true; else state->notify();
}
void gotCutText(rfbClient* client, const char* text, int length) {
    auto* state = stateOf(client);
    if (length < 0 || length > 1 << 20) return;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->remoteClipboard = QString::fromLatin1(text, length);
    }
    state->notify();
}
char* getPassword(rfbClient* client) {
    auto* state = stateOf(client);
    auto credentials = state->credentials(VncWorker::Prompt::Password);
    if (!credentials) return nullptr;
    state->credentialsUsed = true;
    return duplicate(credentials->second);
}
rfbCredential* getCredential(rfbClient* client, int type) {
    auto* state = stateOf(client);
    auto* credential = static_cast<rfbCredential*>(std::calloc(1, sizeof(rfbCredential)));
    if (!credential) return nullptr;
    if (type == rfbCredentialTypeX509) {
        // OpenSSL's default paths first; verifyServerCertificate adds the platform store,
        // imported authorities, pinned certificates and the user's review.
        credential->x509Credential.x509CrlVerifyMode = rfbX509CrlVerifyNone;
        return credential;
    }
    if (type != rfbCredentialTypeUser) { std::free(credential); return nullptr; }
    state->plainRequested = true;
    auto credentials = state->credentials(VncWorker::Prompt::UsernamePassword);
    if (!credentials) { std::free(credential); return nullptr; }
    state->credentialsUsed = true;
    credential->userCredential.username = duplicate(credentials->first.toUtf8());
    credential->userCredential.password = duplicate(credentials->second);
    return credential;
}
QByteArray derOf(X509* certificate) {
    const int size = certificate ? i2d_X509(certificate, nullptr) : 0;
    if (size <= 0) return {};
    QByteArray der(size, Qt::Uninitialized); auto* out = reinterpret_cast<unsigned char*>(der.data());
    return i2d_X509(certificate, &out) == size ? der : QByteArray();
}
// OpenSSL's verify callback for the X509 types. OpenSSL calls it for every certificate in
// the chain; the first failure decides for the whole connection, as a browser would:
// platform trust store or imported authority, then a pinned certificate, then the user.
int verifyServerCertificate(int ok, X509_STORE_CTX* context) {
    auto* ssl = static_cast<SSL*>(X509_STORE_CTX_get_ex_data(context, SSL_get_ex_data_X509_STORE_CTX_idx()));
    auto* client = ssl ? static_cast<rfbClient*>(SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl))) : nullptr;
    auto* state = client ? stateOf(client) : nullptr;
    if (!state) return ok;
    if (state->certificateAccepted) return *state->certificateAccepted ? 1 : 0;
    if (ok) return 1;
    const auto leaf = derOf(X509_STORE_CTX_get0_cert(context));
    const auto certificate = describeVncCertificate(leaf);
    if (!certificate) { state->certificateAccepted = false; return 0; }
    QList<QByteArray> chain;
    if (auto* sent = X509_STORE_CTX_get0_untrusted(context))
        for (int i = 0; i < sk_X509_num(sent); ++i) if (auto der = derOf(sk_X509_value(sent, i)); !der.isEmpty() && der != leaf) chain << der;
    const auto verification = verifyVncCertificate(state->host, leaf, chain, state->authorities);
    if (verification.trusted) { state->certificateIdentity = "certificate verified"; state->certificateAccepted = true; return 1; }
    if (state->pinned && state->pinned->sha256 == certificate->sha256) {
        state->certificateIdentity = "trusted certificate"; state->certificateAccepted = true; return 1;
    }
    VncCertificateReview review{state->host, state->port, *certificate, verification.reason, {}};
    if (state->pinned) review.previous = state->pinned;
    const auto answer = state->reviewCertificate(std::move(review));
    const bool accepted = answer && *answer != VncWorker::CertificateTrust::Reject;
    state->certificateRejected = answer.has_value() && !accepted;
    state->certificateIdentity = answer == VncWorker::CertificateTrust::Always ? "certificate trusted now" : "certificate accepted for this session";
    state->certificateAccepted = accepted;
    return accepted ? 1 : 0;
}
void installHooks() {
    static std::once_flag once;
    std::call_once(once, [] { rfbClientLog = logLine; rfbClientErr = logLine; rfbClientTlsVerify = verifyServerCertificate; });
}

QString handshakeFailure(const VncWorker::State& state) {
    if (state.certificateRejected) return "The VNC server certificate was not trusted, so the connection was canceled.";
    if (const auto* line = loggedLine("Could not verify server certificate: "))
        return "The VNC server certificate could not be verified (" + QString::fromStdString(line->substr(37)).chopped(line->back() == '.' ? 1 : 0) + ").";
    if (loggedLine("Unknown authentication scheme") || loggedLine("Unknown VeNCrypt authentication scheme") || loggedLine("Unknown sub authentication scheme"))
        return "The VNC server requires a security type CMRemote does not support (it supports None, VncAuth and VeNCrypt).";
    const auto* reason = loggedLine("VNC connection failed: ");
    if (state.credentialsUsed && (reason || loggedLine("VNC authentication failed")))
        return QString(state.plainRequested ? "The VNC server rejected the username or password." : "The VNC server rejected the password.") +
            (reason ? " (" + QString::fromStdString(reason->substr(23)) + ")" : QString());
    if (reason) return "The VNC server refused the connection: " + QString::fromStdString(reason->substr(23));
    if (loggedLine("Server refused VeNCrypt")) return "The VNC server refused the VeNCrypt security type.";
    if (loggedLine("Not a valid VNC server")) return "The server is not a VNC server.";
    if (!recentLog.empty()) return "The VNC handshake failed: " + QString::fromStdString(recentLog.back());
    return "The VNC handshake failed.";
}

// The transport thread: resolve wait, connect, LibVNCClient handshake and message loop.
void runTransport(const std::shared_ptr<VncWorker::State>& state) {
    using Phase = VncWorker::Phase;
    prepareVncTransportThread(); installHooks();
    QString failure; bool cancelled = false;
    rfbClient* client = nullptr;
    const auto finish = [&] {
        if (client) {
            {
                // Never shut down a closed (possibly reused) descriptor from the GUI thread.
                std::lock_guard<std::mutex> lock(state->mutex); state->socket = invalidVncSocket();
            }
            client->frameBuffer = nullptr; // owned by VncGraphics
            rfbClientCleanup(client); client = nullptr;
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        state->password.reset(); state->answeredPassword.reset(); state->commands.clear();
        if (!state->abortReason.isEmpty()) { state->phase = Phase::Failed; state->error = state->abortReason; }
        else if (cancelled || state->cancelled.load()) state->phase = Phase::Cancelled;
        else if (failure.isEmpty()) state->phase = Phase::Disconnected;
        else { state->phase = Phase::Failed; state->error = failure; }
    };
    if (!state->wake.valid()) { failure = "The VNC worker could not start."; finish(); return; }
    QStringList addresses;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->changed.wait(lock, [&] { return state->cancelled.load() || state->addresses.has_value(); });
        if (state->addresses) addresses = *state->addresses;
    }
    if (state->cancelled.load()) { cancelled = true; finish(); return; }
    if (addresses.isEmpty()) { failure = "The VNC hostname could not be resolved."; finish(); return; }
    state->setPhase(Phase::Connecting);
    const auto socket = connectVncSocket(addresses, state->port, 5000, state->wake, state->cancelled);
    if (socket == invalidVncSocket()) {
        if (state->cancelled.load()) cancelled = true; else failure = "The VNC server could not be reached.";
        finish(); return;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->cancelled.load()) { closeVncSocket(socket); cancelled = true; }
        else state->socket = socket;
    }
    if (cancelled) { finish(); return; }
    state->setPhase(Phase::Handshaking);
    client = rfbGetClient(8, 3, 4);
    if (!client) { closeVncSocket(socket); { std::lock_guard<std::mutex> lock(state->mutex); state->socket = invalidVncSocket(); } failure = "The VNC worker could not start."; finish(); return; }
    client->sock = decltype(client->sock)(socket);
    rfbClientSetClientData(client, &ClientTag, state.get());
    std::free(client->serverHost);
    // X509 types verify the certificate against the name the user entered.
    client->serverHost = duplicate(state->host.toUtf8()); client->serverPort = state->port;
    // B,G,R,X in memory: QImage::Format_RGB32 on little-endian hosts.
    client->format.redShift = 16; client->format.greenShift = 8; client->format.blueShift = 0;
    client->appData.shareDesktop = TRUE; client->appData.useRemoteCursor = TRUE; client->appData.enableJPEG = FALSE;
    client->appData.encodingsString = "zrle tight trle copyrect hextile zlib raw";
    client->appData.compressLevel = 6;
    client->canHandleNewFBSize = TRUE;
    // Bounds a server that stalls mid-message; idle desktops never time out.
    client->readTimeout = 30;
    client->MallocFrameBuffer = mallocFramebuffer;
    client->GotFrameBufferUpdate = gotUpdate;
    client->FinishedFrameBufferUpdate = finishedUpdate;
    client->GotCursorShape = gotCursor;
    client->GotXCutText = gotCutText;
    client->GetPassword = getPassword;
    client->GetCredential = getCredential;
    // ARD and MS-Logon need crypto this build omits; anonymous TLS and VeNCrypt carry their own subtypes.
    const uint32_t schemes[] = {rfbNoAuth, rfbVncAuth, rfbVeNCrypt, rfbTLS, 0};
    SetClientAuthSchemes(client, schemes, -1);
    const bool initialized = InitialiseRFBConnection(client);
    if (state->cancelled.load()) { cancelled = true; finish(); return; }
    if (!initialized) { failure = handshakeFailure(*state); finish(); return; }
    client->width = client->si.framebufferWidth; client->height = client->si.framebufferHeight;
    if (!client->MallocFrameBuffer(client)) { failure = "The VNC desktop size exceeds supported limits."; finish(); return; }
    client->updateRect = {0, 0, client->width, client->height}; client->isUpdateRectManagedByLib = TRUE;
    if (!SetFormatAndEncodings(client) || !SendFramebufferUpdateRequest(client, 0, 0, client->width, client->height, FALSE)) {
        if (state->cancelled.load()) cancelled = true; else failure = "The VNC server closed the connection during setup.";
        finish(); return;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->security = securityName(client->authScheme, client->subAuthScheme);
        state->encrypted = client->tlsSession != nullptr;
        state->identity = state->certificateIdentity;
    }
    state->setPhase(Phase::Connected);
    using Clock = std::chrono::steady_clock;
    std::optional<Clock::time_point> resizeSent;
    while (!state->cancelled.load()) {
        std::deque<VncCommand> batch;
        { std::lock_guard<std::mutex> lock(state->mutex); batch.swap(state->commands); }
        bool sent = true;
        const auto geometry = state->geometry.load();
        for (auto& command : batch) {
            switch (command.kind) {
            case VncCommand::Kind::Key: sent = SendKeyEvent(client, command.keysym, command.down ? TRUE : FALSE); break;
            case VncCommand::Kind::Pointer:
                // A server-driven shrink can race GUI pointer mapping; clamp, never fail.
                sent = SendPointerEvent(client, qMin<int>(command.x, int(geometry >> 16) - 1), qMin<int>(command.y, int(geometry & 0xffff) - 1), command.mask);
                break;
            case VncCommand::Kind::Clipboard: sent = SendClientCutText(client, command.text.data(), int(command.text.size())); break;
            case VncCommand::Kind::Resize:
                if (client->screen.width) { sent = SendExtDesktopSize(client, quint16(command.size.width()), quint16(command.size.height())); resizeSent = Clock::now(); }
                break;
            }
            if (!sent) break;
        }
        if (!sent) { if (!state->cancelled.load()) failure = "The VNC connection was lost while sending input."; break; }
        // Bytes already buffered by LibVNCClient or OpenSSL never wake poll().
        const bool buffered = client->buffered > 0 || (client->tlsSession && SSL_pending(static_cast<SSL*>(client->tlsSession)) > 0);
        if (!buffered) {
            const auto wait = waitVncSocket(socket, state->wake, 1000);
            if (wait == VncWait::Woken) { state->wake.drain(); continue; }
            if (wait == VncWait::Error) { failure = "The VNC connection failed."; break; }
            if (wait == VncWait::Timeout) {
                // A server that ignores SetDesktopSize must not freeze updates forever.
                if (client->requestedResize && resizeSent && Clock::now() - *resizeSent > std::chrono::seconds(3)) {
                    client->requestedResize = FALSE; resizeSent.reset();
                    if (!SendIncrementalFramebufferUpdateRequest(client)) { failure = "The VNC connection was lost."; break; }
                }
                continue;
            }
        }
        if (!HandleRFBServerMessage(client)) {
            if (state->graphicsFailed) failure = "VNC graphics exceed supported bounds.";
            else if (!state->cancelled.load() && !loggedLine("VNC server closed connection"))
                failure = recentLog.empty() ? "The VNC server sent invalid or unsupported data." :
                    "The VNC connection failed: " + QString::fromStdString(recentLog.back());
            break;
        }
        if (state->graphicsFailed) { failure = "VNC graphics exceed supported bounds."; break; }
        if (!client->requestedResize) resizeSent.reset();
        if (client->screen.width && !state->resizable && !state->viewOnly) {
            { std::lock_guard<std::mutex> lock(state->mutex); state->resizable = true; }
            state->notify();
        }
    }
    if (state->cancelled.load()) cancelled = true;
    finish();
}
}

VncWorker::VncWorker(SessionTarget target, VncAuthentication authentication, quint64 generation, Options options, QObject* parent)
    : QObject(parent), state_(std::make_shared<State>()), generation_(generation) {
    setObjectName("vncWorker");
    installHooks();
    host_ = target.host;
    state_->host = target.host; state_->port = target.port; state_->viewOnly = target.viewOnly;
    state_->authorities = std::move(options.authorities); state_->pinned = std::move(options.pinned); state_->generation = generation;
    state_->password = std::move(authentication.password); state_->username = options.username;
    auto* delivery = new QObject; delivery->setObjectName("vncWorkerDelivery"); state_->delivery = delivery;
    const auto owner = QPointer<VncWorker>(this); const auto weak = std::weak_ptr<State>(state_);
    state_->notify = [weak, owner, delivery] {
        const auto state = weak.lock();
        if (!state || state->notifyPending.exchange(true)) return;
        // The retained delivery object survives pane destruction and native join.
        QMetaObject::invokeMethod(delivery, [weak, owner] {
            if (const auto state = weak.lock()) drain(state, owner);
        }, Qt::QueuedConnection);
    };
    const auto state = state_;
    auto* worker = QThread::create([state] { runTransport(state); state->notify(); });
    state->worker = worker; worker->setObjectName("vncTransportWorker");
    connect(worker, &QThread::finished, delivery, [state, owner, delivery, worker] {
        // finished can precede native TLS cleanup. Never wait on the GUI thread.
        std::thread([state, owner, delivery, worker] {
            worker->wait();
            QMetaObject::invokeMethod(delivery, [state, owner, worker] {
                state->joined = true; state->worker = nullptr;
                worker->deleteLater(); drain(state, owner);
            }, Qt::QueuedConnection);
        }).detach();
    }, Qt::QueuedConnection);
    ++liveWorkers; worker->start();
    // Connection and handshake stages are bounded; time spent in the credential and certificate prompts is not.
    deadline_.setSingleShot(true); deadline_.setInterval(StageTimeoutMs);
    connect(&deadline_, &QTimer::timeout, this, [this] { state_->abort("The VNC connection timed out."); });
    deadline_.start();
    lookup_ = QHostInfo::lookupHost(target.host, this, [this, generation](const QHostInfo& info) {
        lookup_ = -1;
        if (generation != generation_ || state_->stopping || state_->retired) return;
        QStringList addresses; QSet<QString> seen;
        if (info.error() == QHostInfo::NoError) {
            for (const auto& address : info.addresses()) {
                const auto value = address.toString();
                if (exposed_.isEmpty() && !localNetwork(address)) exposed_ = value;
                if (seen.contains(value)) continue;
                seen.insert(value); addresses.push_back(value); if (addresses.size() == 16) break;
            }
        }
        { std::lock_guard<std::mutex> lock(state_->mutex); state_->addresses = addresses; }
        state_->changed.notify_all();
    });
}
VncWorker::~VncWorker() { stop(); }
int VncWorker::activeWorkers() { return liveWorkers.load(); }
void VncWorker::abortLookup() { if (lookup_ >= 0) { QHostInfo::abortHostLookup(lookup_); lookup_ = -1; } }
void VncWorker::stop() {
    Q_ASSERT(QThread::currentThread() == thread()); abortLookup(); deadline_.stop();
    state_->stopping = true;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->cancelled = true; shutdownVncSocket(state_->socket);
    }
    state_->changed.notify_all(); state_->wake.signal();
}
bool VncWorker::enqueue(VncCommand command, QString* error) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (state_->stopping || state_->retired || phase_ != Phase::Connected) return false;
    // View-only sessions never change the server: no input, clipboard or resize.
    if (state_->viewOnly) { if (error) *error = "This VNC session is view-only."; return false; }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto& commands = state_->commands;
        // Motion that follows motion with the same buttons, and repeated clipboard/resize
        // requests, replace the pending one; key and button transitions are never dropped.
        if (!commands.empty() && commands.back().kind == command.kind &&
            ((command.kind == VncCommand::Kind::Pointer && command.motion && commands.back().motion) ||
             command.kind == VncCommand::Kind::Clipboard || command.kind == VncCommand::Kind::Resize)) {
            commands.back() = std::move(command);
        } else if (commands.size() >= InputCapacity) {
            if (error) *error = "The VNC input queue is full; the connection was stopped to preserve input transitions.";
            state_->abortReason = error ? *error : QString("The VNC input queue is full.");
            state_->cancelled = true; shutdownVncSocket(state_->socket);
            return false;
        } else commands.push_back(std::move(command));
    }
    state_->wake.signal();
    return true;
}
bool VncWorker::key(quint32 keysym, bool down, quint64 generation, QString* error) {
    if (generation != generation_) return false;
    if (keysym == 0) { if (error) *error = "VNC input is outside the supported bounds."; return false; }
    VncCommand command{VncCommand::Kind::Key}; command.keysym = keysym; command.down = down;
    return enqueue(std::move(command), error);
}
bool VncWorker::pointer(quint8 mask, quint16 x, quint16 y, quint64 generation, QString* error) {
    if (generation != generation_) return false;
    VncCommand command{VncCommand::Kind::Pointer}; command.mask = mask; command.x = x; command.y = y;
    command.motion = pointerMask_ && *pointerMask_ == mask;
    if (!enqueue(std::move(command), error)) return false;
    pointerMask_ = mask; return true;
}
void VncWorker::resizeDesktop(QSize size) {
    if (!resizable_ || size.isEmpty()) return;
    VncCommand command{VncCommand::Kind::Resize};
    command.size = QSize(qBound(64, size.width(), VncGraphics::MaxSide), qBound(64, size.height(), VncGraphics::MaxSide));
    enqueue(std::move(command), nullptr);
}
void VncWorker::configureClipboard(ClipboardControl control) {
    Q_ASSERT(QThread::currentThread() == thread());
    clipboard_ = control;
    if (!control.enabled) { std::lock_guard<std::mutex> lock(state_->mutex); state_->remoteClipboard.reset(); }
}
void VncWorker::clipboardText(QString text, ClipboardStamp stamp) {
    if (!clipboard_.enabled || stamp.permission != clipboard_.stamp.permission || text.size() > 512 * 1024 - 1) return;
    VncCommand command{VncCommand::Kind::Clipboard}; command.text = latin1CutText(text);
    enqueue(std::move(command), nullptr);
}
std::optional<ClipboardDelivery> VncWorker::takeClipboard() {
    Q_ASSERT(QThread::currentThread() == thread());
    state_->clipboardPosted = false;
    std::optional<QString> text;
    { std::lock_guard<std::mutex> lock(state_->mutex); text = std::move(state_->remoteClipboard); state_->remoteClipboard.reset(); }
    // Stamped with the latest local route/revision; SessionClipboard rejects stale ones.
    if (!text || !clipboard_.enabled) return std::nullopt;
    return ClipboardDelivery{std::move(*text), clipboard_.stamp};
}
bool VncWorker::credentials(QString username, std::optional<Secret> password, quint64 generation, QString* error) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (generation != generation_ || state_->stopping || state_->retired || phase_ != Phase::PasswordRequired) return false;
    if (password && (password->size() == 0 || password->size() > VncAuthentication::MaxPassword ||
                     std::memchr(password->data(), 0, password->size()))) {
        if (error) *error = "VNC passwords must be 1–4096 bytes without NUL characters.";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->waitingCredentials) return false;
        if (!password && !state_->password) { if (error) *error = "Enter the VNC password."; return false; }
        if (state_->prompt == Prompt::UsernamePassword) {
            if (username.isEmpty()) { if (error) *error = "Enter the VNC username."; return false; }
            state_->username = std::move(username);
        }
        state_->answeredPassword = std::move(password); state_->credentialsAnswered = true;
    }
    state_->changed.notify_all();
    deadline_.start();
    return true;
}
bool VncWorker::trustCertificate(CertificateTrust trust, quint64 generation) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (generation != generation_ || state_->stopping || state_->retired || phase_ != Phase::CertificateReview) return false;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->waitingCertificate) return false;
        state_->certificateAnswer = trust; state_->certificateAnswered = true;
    }
    state_->changed.notify_all();
    deadline_.start();
    return true;
}
DesktopUpdate VncWorker::takeUpdate() {
    Q_ASSERT(QThread::currentThread() == thread());
    state_->framePosted = false; return state_->graphics.takeUpdate();
}
std::optional<RemoteCursor> VncWorker::takeCursor() {
    Q_ASSERT(QThread::currentThread() == thread());
    state_->cursorPosted = false; return state_->graphics.takeCursor();
}
void VncWorker::drain(const std::shared_ptr<State>& state, const QPointer<VncWorker>& owner) {
    state->notifyPending = false;
    if (state->retired) return;
    Phase phase; QString error, security, identity; Prompt prompt; QString username; bool encrypted, resizable, clipboard;
    VncCertificateReview review;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        phase = state->phase; error = state->error; security = state->security; identity = state->identity; prompt = state->prompt;
        if (phase == Phase::CertificateReview) review = state->review;
        username = state->username; encrypted = state->encrypted; resizable = state->resizable;
        clipboard = state->remoteClipboard.has_value();
    }
    const auto current = [&] { return owner && owner->state_ == state && owner->generation_ == state->generation; };
    if (current()) {
        if (owner->phase_ != phase) {
            owner->phase_ = phase; owner->prompt_ = prompt; owner->promptUsername_ = username;
            owner->security_ = security; owner->encrypted_ = encrypted; owner->identity_ = identity;
            if (phase == Phase::CertificateReview) owner->review_ = review;
            if (phase == Phase::PasswordRequired || phase == Phase::CertificateReview) owner->deadline_.stop();
            if (phase == Phase::Connected || terminal(phase)) owner->deadline_.stop();
            if (terminal(phase)) owner->abortLookup();
            if (phase == Phase::Connected && !encrypted && !owner->exposed_.isEmpty())
                emit owner->securityWarning("VNC to " + owner->host_ + " (" + owner->exposed_ + ") uses " + security +
                    ", which crosses the internet unencrypted: the screen, keystrokes and password can be read in transit. "
                    "Use a VeNCrypt TLS type, a VPN or an SSH tunnel outside your local network.", state->generation);
            emit owner->phaseChanged(phase, error, state->generation);
        }
        if (current() && owner->resizable_ != resizable) {
            owner->resizable_ = resizable; emit owner->resizeAvailable(resizable, state->generation);
        }
        if (current() && !state->stopping && !state->framePosted && state->graphics.hasUpdate()) {
            state->framePosted = true; emit owner->frameAvailable(state->generation);
        }
        if (current() && !state->stopping && !state->cursorPosted && state->graphics.hasCursor()) {
            state->cursorPosted = true; emit owner->cursorAvailable(state->generation);
        }
        if (current() && !state->stopping && !state->clipboardPosted && clipboard) {
            state->clipboardPosted = true; emit owner->clipboardAvailable(state->generation);
        }
    }
    if (state->joined && terminal(phase)) {
        // With the producer joined no further snapshots can arrive. Discard any
        // final unconsumed mailboxes before releasing the retained delivery context.
        state->graphics.takeUpdate(); state->graphics.takeCursor();
        state->retired = true; --liveWorkers;
        if (current()) emit owner->finished(state->generation);
        state->delivery->deleteLater();
    }
}
