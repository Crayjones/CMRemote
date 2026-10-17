#pragma once
#include "RdpTarget.h"
#include "SshHostKeyStore.h"
#include "remotemanager-core/src/ssh.rs.h"
#include <QObject>
#include <QByteArray>
#include <QPointer>
#include <memory>

class SshWorker final : public QObject {
    Q_OBJECT
public:
    enum class Phase { Resolving, Connecting, Handshaking, WaitingTrust, Authenticating, Opening, Connected, Disconnected, Cancelled, Failed };
    Q_ENUM(Phase)
    // A nonempty command runs without a PTY instead of an interactive shell.
    SshWorker(SessionTarget target, rust::Box<rm::ssh::Authentication> authentication, quint64 generation, QObject* parent = nullptr, const QString& command = {});
    ~SshWorker() override;
    Phase phase() const { return phase_; }
    quint64 generation() const { return generation_; }
    bool input(const QByteArray& bytes, quint64 generation, QString* error = nullptr);
    bool resize(quint16 rows, quint16 columns, quint64 generation);
    bool answerHost(const QString& request, quint64 generation, bool allowed);
    void stop();
    static int activeWorkers();
signals:
    // GUI delivery only. Byte consumers must parse directly, not queue another copy.
    void output(QByteArray bytes, quint64 generation);
    void phaseChanged(SshWorker::Phase phase, QString error, quint64 generation);
    void hostKey(QString request, SshHostKey key, quint64 generation);
    // Native thread has been joined off GUI and retained output has been drained.
    void finished(quint64 generation);
private:
    struct State;
    static void drain(const std::shared_ptr<State>& state, const QPointer<SshWorker>& owner);
    void abortLookup();
    std::shared_ptr<State> state_;
    const quint64 generation_;
    Phase phase_ = Phase::Resolving;
    int lookup_ = -1;
};
