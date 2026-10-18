#pragma once
#include "RdpTarget.h"
#include "DesktopUpdate.h"
#include "RemoteCursor.h"
#include "Secret.h"
#include "remotemanager-core/src/vnc.rs.h"
#include <QObject>
#include <QPointer>
#include <memory>

class VncWorker final : public QObject {
    Q_OBJECT
public:
    // Matches the Rust transport order. PasswordRequired waits for password().
    enum class Phase { Resolving, Connecting, Handshaking, Connected, Disconnected, Cancelled, Failed, PasswordRequired };
    Q_ENUM(Phase)
    VncWorker(SessionTarget target, rust::Box<rm::vnc::Authentication> authentication, quint64 generation, QObject* parent = nullptr);
    ~VncWorker() override;
    Phase phase() const { return phase_; }
    quint64 generation() const { return generation_; }
    bool key(quint32 keysym, bool down, quint64 generation, QString* error = nullptr);
    bool pointer(quint8 mask, quint16 x, quint16 y, quint64 generation, QString* error = nullptr);
    bool password(Secret password, quint64 generation, QString* error = nullptr);
    DesktopUpdate takeUpdate();
    std::optional<RemoteCursor> takeCursor();
    void stop();
    static int activeWorkers();
signals:
    void frameAvailable(quint64 generation);
    void cursorAvailable(quint64 generation);
    void phaseChanged(VncWorker::Phase phase, QString error, quint64 generation);
    // The host resolved to a public internet address; classic VNC is unencrypted.
    void securityWarning(QString text, quint64 generation);
    // Native thread has been joined off GUI and all retained deliveries drained.
    void finished(quint64 generation);
private:
    struct State;
    static void drain(const std::shared_ptr<State>& state, const QPointer<VncWorker>& owner);
    void abortLookup();
    std::shared_ptr<State> state_;
    const quint64 generation_;
    Phase phase_ = Phase::Resolving;
    int lookup_ = -1;
};
