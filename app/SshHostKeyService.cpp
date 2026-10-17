#include "SshHostKeyService.h"
#include <QPointer>
#include <QThread>
#include <QUuid>
#include <atomic>
#include <thread>

namespace {
std::atomic<int> liveWorkers{0};
constexpr int maxJobs = 1024;
void canceled(SshHostKeyReply& reply) {
    reply.allowed = false; reply.canceled = true;
    if (!reply.committed) reply.result = {false, "SSH host-key request canceled.", {}};
}
}
struct SshHostKeyService::Slot {
    explicit Slot(quint64 value) : generation(value) {}
    const quint64 generation;
    std::atomic<bool> active{true};
    bool busy = false; // GUI-owned; ticket is worker-owned.
    QString ticket;
};
struct SshHostKeyService::State {
    QString path;
    Options options;
    std::unique_ptr<SshHostKeyStore> store;
    SshHostKeyStore& get() {
        if (!store) { store = std::make_unique<SshHostKeyStore>(path); store->setFault(options.fault); }
        return *store;
    }
};
SshHostKeyService::SshHostKeyService(QString path, QObject* parent) : SshHostKeyService(std::move(path), Options{}, parent) {}
SshHostKeyService::SshHostKeyService(QString path, Options options, QObject* parent)
    : QObject(parent), state_(std::make_shared<State>()) {
    state_->path = std::move(path); state_->options = std::move(options); setObjectName("sshHostKeys");
    workerThread_ = new QThread; workerThread_->setObjectName("sshHostKeysWorker");
    worker_ = new QObject; worker_->moveToThread(workerThread_);
    // This sink outlives the service and every worker functor, including queued replies.
    delivery_ = new QObject; const auto owner = QPointer<SshHostKeyService>(this); auto* delivery = delivery_;
    connect(workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    auto* workerThread = workerThread_;
    connect(workerThread, &QThread::finished, delivery, [owner, delivery, workerThread] {
        // QThread::finished precedes native thread-local cleanup. Join off GUI.
        std::thread([owner, delivery, workerThread] {
            workerThread->wait();
            QMetaObject::invokeMethod(delivery, [owner, delivery, workerThread] {
                --liveWorkers; workerThread->deleteLater();
                if (owner) {
                    owner->workerThread_ = nullptr; owner->worker_ = nullptr; owner->delivery_ = nullptr; owner->finished_ = true;
                    auto completions = std::move(owner->shutdownCompletions_);
                    for (auto& complete : completions) complete();
                }
                delivery->deleteLater();
            }, Qt::QueuedConnection);
        }).detach();
    }, Qt::QueuedConnection);
    ++liveWorkers; workerThread_->start();
}
SshHostKeyService::~SshHostKeyService() { shutdown(); }
int SshHostKeyService::activeWorkers() { return liveWorkers.load(); }
void SshHostKeyService::reject(quint64 generation, QString error, Completion complete) {
    QMetaObject::invokeMethod(this, [generation, error = std::move(error), complete = std::move(complete)]() mutable {
        SshHostKeyReply reply; reply.generation = generation; reply.result.error = std::move(error); reply.conflict = true; complete(std::move(reply));
    }, Qt::QueuedConnection);
}
QString SshHostKeyService::check(SshHostKey candidate, quint64 generation, Completion complete) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (shuttingDown_ || jobs_ >= maxJobs || slots_.size() >= maxJobs || candidate.host.size() > 4096 || candidate.key.size() > 16 * 1024 || candidate.algorithm.size() > 128 || candidate.fingerprint.size() > 128) {
        reject(generation, "SSH host-key service stopped or request exceeds supported limits.", std::move(complete)); return {};
    }
    const auto request = QUuid::createUuid().toString(QUuid::WithoutBraces); auto slot = std::make_shared<Slot>(generation); slots_.insert(request, slot);
    execute(request, slot, [candidate = std::move(candidate)](auto& store, auto& slot) {
        auto reply = store.inspect(candidate);
        if (reply.inspection) {
            slot.ticket = reply.inspection->ticket;
            if (reply.inspection->status == SshHostKeyStatus::Trusted) reply = store.decide(slot.ticket, SshHostKeyDecision::UseTrusted);
        }
        return reply;
    }, std::move(complete));
    return request;
}
void SshHostKeyService::decide(const QString& request, quint64 generation, SshHostKeyDecision decision, Completion complete) {
    Q_ASSERT(QThread::currentThread() == thread()); const auto slot = slots_.value(request);
    if (shuttingDown_ || !slot || slot->generation != generation || !slot->active.load() || slot->busy || jobs_ >= maxJobs) {
        reject(generation, "Stale or busy SSH host-key request.", std::move(complete)); return;
    }
    execute(request, slot, [decision](auto& store, auto& slot) { return store.decide(slot.ticket, decision); }, std::move(complete));
}
void SshHostKeyService::execute(const QString& request, const std::shared_ptr<Slot>& slot, Operation operation, Completion complete) {
    slot->busy = true; ++jobs_; const auto state = state_; const auto owner = QPointer<SshHostKeyService>(this); auto* delivery = delivery_;
    QMetaObject::invokeMethod(worker_, [state, owner, delivery, request, slot, operation = std::move(operation), complete = std::move(complete)]() mutable {
        SshHostKeyReply reply;
        if (slot->active.load()) {
            auto& store = state->get(); const auto user = state->options.commitGate;
            store.setCommitGate([slot, user](const auto& commit) {
                const auto guarded = [&] { return slot->active.load() && commit(); };
                return user ? user(guarded) : guarded();
            });
            reply = operation(store, *slot); store.setCommitGate({});
        } else canceled(reply);
        if (!slot->active.load()) { canceled(reply); if (state->store) state->store->cancel(slot->ticket); }
        reply.generation = slot->generation; reply.performedOn = QThread::currentThreadId();
        QMetaObject::invokeMethod(delivery, [owner, request, slot, reply = std::move(reply), complete = std::move(complete)]() mutable {
            if (!owner) return;
            --owner->jobs_; slot->busy = false;
            if (!slot->active.load() || owner->slots_.value(request) != slot) {
                canceled(reply); if (!owner->shuttingDown_) owner->discard(slot);
            }
            if (reply.allowed || reply.canceled || reply.conflict || !reply.inspection) { owner->slots_.remove(request); slot->active = false; }
            complete(std::move(reply));
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}
void SshHostKeyService::discard(const std::shared_ptr<Slot>& slot) {
    ++jobs_; const auto state = state_; const auto owner = QPointer<SshHostKeyService>(this); auto* delivery = delivery_;
    QMetaObject::invokeMethod(worker_, [state, owner, delivery, slot] {
        if (state->store) state->store->cancel(slot->ticket);
        QMetaObject::invokeMethod(delivery, [owner] { if (owner) --owner->jobs_; }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}
void SshHostKeyService::cancel(const QString& request) {
    Q_ASSERT(QThread::currentThread() == thread()); const auto slot = slots_.take(request); if (!slot) return;
    slot->active = false; if (!slot->busy && !shuttingDown_) discard(slot);
}
void SshHostKeyService::shutdown(std::function<void()> complete) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (finished_) { if (complete) QMetaObject::invokeMethod(this, std::move(complete), Qt::QueuedConnection); return; }
    if (complete) shutdownCompletions_.push_back(std::move(complete));
    if (shuttingDown_) return;
    shuttingDown_ = true; for (const auto& slot : slots_) slot->active = false; slots_.clear();
    const auto state = state_;
    QMetaObject::invokeMethod(worker_, [state] { state->store.reset(); QThread::currentThread()->quit(); }, Qt::QueuedConnection);
}
