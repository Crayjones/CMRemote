#pragma once
#include "SshHostKeyStore.h"
#include <QObject>
#include <functional>
#include <memory>

class QThread;
class SshHostKeyService final : public QObject {
    Q_OBJECT
public:
    using Completion = std::function<void(SshHostKeyReply)>;
    struct Options { AtomicFile::Fault fault = AtomicFile::Fault::None; AtomicFile::CommitGate commitGate; };
    explicit SshHostKeyService(QString path, QObject* parent = nullptr);
    SshHostKeyService(QString path, Options options, QObject* parent = nullptr);
    ~SshHostKeyService() override;
    QString check(SshHostKey candidate, quint64 generation, Completion complete);
    void decide(const QString& request, quint64 generation, SshHostKeyDecision decision, Completion complete);
    void cancel(const QString& request);
    void shutdown(std::function<void()> complete = {});
    static int activeWorkers();
private:
    struct State;
    struct Slot;
    using Operation = std::function<SshHostKeyReply(SshHostKeyStore&, Slot&)>;
    void execute(const QString& request, const std::shared_ptr<Slot>& slot, Operation operation, Completion complete);
    void discard(const std::shared_ptr<Slot>& slot);
    void reject(quint64 generation, QString error, Completion complete);
    QThread* workerThread_ = nullptr;
    QObject* worker_ = nullptr;
    QObject* delivery_ = nullptr;
    std::shared_ptr<State> state_;
    QHash<QString, std::shared_ptr<Slot>> slots_;
    QVector<std::function<void()>> shutdownCompletions_;
    int jobs_ = 0;
    bool shuttingDown_ = false, finished_ = false;
};
