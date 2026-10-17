#pragma once
#include "RdpTarget.h"
#include "RdpGraphicsPolicy.h"
#include "PerformanceTrace.h"
#include "DesktopUpdate.h"
#include "RdpInputQueue.h"
#include "RemoteCursor.h"
#include "Credentials.h"
#include <memory>
#include <QImage>
#include <QSize>
#include <QThread>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

struct RdpConfig {
    QString host, username, configDirectory;
    QByteArray password;
    quint16 port = 3389;
    bool clipboard = false;
    QString domain;
    RdpOptions options;
    QSize panelSize{1024, 768};
    std::shared_ptr<PerformanceTrace> performance;
    RdpGraphicsPolicy graphicsPolicy = RdpGraphicsPolicy::Gfx;
};
struct rdp_context;
struct rdp_settings;
class RdpWorker final : public QThread {
    Q_OBJECT
public:
    // Last negotiated desktop; valid once desktopNegotiated has been emitted.
    QSize negotiatedDesktop() const { const auto value = negotiated_.load(); return QSize(int(value >> 32), int(value & 0xffffffffu)); }
    quint32 negotiatedColorDepth() const { return negotiatedDepth_.load(); }
    explicit RdpWorker(RdpConfig config, QObject* parent = nullptr);
    ~RdpWorker() override;
    void stop();
    void key(quint32 scan, bool down, bool repeat);
    void text(QString text);
    void pointer(quint16 flags, quint16 x, quint16 y);
    // Sent only when the server advertised relative mouse input.
    void relativePointer(quint16 flags, qint16 dx, qint16 dy);
    void resizeDesktop(QSize size);
    void localClipboard(QString text);
    void clipboardText(QString text, ClipboardStamp stamp);
    void configureClipboard(ClipboardControl control);
    // Answer to credentialsRequired; nullopt cancels the connection.
    void provideCredentials(std::optional<TransientCredentials> credentials);
    DesktopUpdate takeUpdate();
    QImage takeFrame();
    std::optional<RemoteCursor> takeCursor() { return pendingCursor_.take(); }
    std::optional<ClipboardDelivery> takeClipboard();
    FrameStamp takenFrameStamp();
    static QImage copyFramebuffer(const uchar* bytes, int width, int height, int stride);
    static bool applyProfileSettings(rdp_settings* settings, const SessionTarget& target, QSize panelSize);
signals:
    void frameAvailable();
    void cursorAvailable();
    void stateChanged(QString state, QString detail);
    void remoteClipboard(QString text);
    void clipboardAvailable();
    // FreeRDP asked for credentials during connection setup (gateway or server).
    void credentialsRequired(bool gateway, QString endpoint, QString username, QString domain);
    void resizeAvailable(bool available);
    void desktopNegotiated(QSize size, quint32 colorDepth);
    void relativePointerAvailable(bool available);
    // Server moved the cursor (SetCursorPos on the remote), in desktop pixels.
    void remotePointerMoved(QPoint position);
protected:
    void run() override;
private:
    struct Context;
    using Command = RdpInputQueue::Command;
    void enqueue(Command command);
    void checkInputResult(RdpInputQueue::Result result);
    bool publish(const uchar* bytes, QSize size, int stride, const QRegion& damage, FrameStamp stamp);
    std::optional<TransientCredentials> credentials(bool gateway, const QString& endpoint, const QString& username, const QString& domain);
    RdpConfig config_;
    std::atomic_bool stopping_{false};
    std::atomic<quint64> negotiated_{0}; // width << 32 | height, set before desktopNegotiated
    std::atomic<quint32> negotiatedDepth_{0};
    std::mutex lifetimeMutex_, frameMutex_, credentialsMutex_;
    rdp_context* context_ = nullptr;
    RdpInputQueue input_;
    DesktopUpdateAccumulator pendingUpdate_;
    RemoteCursorMailbox pendingCursor_;
    std::mutex clipboardMutex_;
    std::optional<ClipboardDelivery> pendingClipboard_;
    bool clipboardNotificationPending_ = false;
    QImage compatibilityFrame_;
    FrameStamp takenStamp_;
    quint64 pendingPublishedNs_ = 0, frameSequence_ = 0;
    std::condition_variable credentialsCv_;
    bool certificateRejected_ = false, credentialsAnswered_ = false;
    std::optional<TransientCredentials> credentialAnswer_;
};
