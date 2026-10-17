#include "SshWorker.h"
#include "rust/cxx.h"
#include <QHostInfo>
#include <QHostAddress>
#include <QSet>
#include <QThread>
#include <atomic>
#include <optional>
#include <thread>

namespace {
std::atomic<int> liveWorkers{0};
rust::String owned(const QString& value) { const auto bytes = value.toUtf8(); return rust::String(bytes.constData(), size_t(bytes.size())); }
QString text(const rust::String& value) { return QString::fromUtf8(value.data(), qsizetype(value.size())); }
}
struct SshWorker::State {
    std::optional<rust::Box<rm::ssh::BackendControl>> control;
    QObject* delivery = nullptr;
    QThread* worker = nullptr;
    quint64 generation = 0;
    bool stopping = false, joined = false, retired = false;
};
SshWorker::SshWorker(SessionTarget target, rust::Box<rm::ssh::Authentication> authentication, quint64 generation, QObject* parent, const QString& command)
    : QObject(parent), state_(std::make_shared<State>()), generation_(generation) {
    setObjectName("sshWorker"); state_->generation = generation;
    auto* delivery = new QObject; delivery->setObjectName("sshWorkerDelivery"); state_->delivery = delivery;
    const auto owner = QPointer<SshWorker>(this); const auto state = state_; const auto weak = std::weak_ptr<State>(state);
    const auto sink = std::make_shared<rm::ssh::EventSink>([weak, owner, delivery](quint64 value) {
        // The delivery context remains alive through the off-GUI join and last drain.
        QMetaObject::invokeMethod(delivery, [weak, owner, value] {
            if (const auto state = weak.lock(); state && state->generation == value) drain(state, owner);
        }, Qt::QueuedConnection);
    });
    rm::ssh::BackendTarget snapshot;
    snapshot.host = owned(target.host); snapshot.port = target.port; snapshot.username = owned(target.username);
    snapshot.auth = owned(target.sshAuth); snapshot.key_path = owned(target.sshKeyPath); snapshot.rows = 24; snapshot.columns = 80;
    snapshot.command = owned(command);
    std::optional<rust::Box<rm::ssh::Backend>> backend;
    try { backend.emplace(rm::ssh::new_backend(std::move(snapshot), std::move(authentication), sink, generation)); }
    catch (...) { delete delivery; throw; }
    state->control.emplace((*backend)->control());
    auto* worker = QThread::create([backend = std::move(*backend)]() mutable { rm::ssh::run_backend(std::move(backend)); });
    state->worker = worker; worker->setObjectName("sshTransportWorker");
    connect(worker, &QThread::finished, delivery, [state, owner, delivery, worker] {
        // QThread::finished can precede native TLS cleanup. The join itself is never
        // performed by the GUI, including when the pane/service has been deleted.
        std::thread([state, owner, delivery, worker] {
            worker->wait();
            QMetaObject::invokeMethod(delivery, [state, owner, worker] {
                state->joined = true; state->worker = nullptr;
                worker->deleteLater(); drain(state, owner);
            }, Qt::QueuedConnection);
        }).detach();
    }, Qt::QueuedConnection);
    ++liveWorkers; worker->start();
    lookup_ = QHostInfo::lookupHost(target.host, this, [this, generation](const QHostInfo& info) {
        lookup_ = -1;
        if (generation != generation_ || state_->stopping || state_->retired || !state_->control) return;
        rust::Vec<rust::String> addresses; QSet<QString> seen;
        if (info.error() == QHostInfo::NoError) {
            for (const auto& address : info.addresses()) {
                const auto value = address.toString();
                if (seen.contains(value)) continue;
                seen.insert(value); addresses.push_back(owned(value)); if (addresses.size() == 16) break;
            }
        }
        (*state_->control)->resolved(std::move(addresses));
    });
}
SshWorker::~SshWorker() { stop(); }
int SshWorker::activeWorkers() { return liveWorkers.load(); }
void SshWorker::abortLookup() { if (lookup_ >= 0) { QHostInfo::abortHostLookup(lookup_); lookup_ = -1; } }
void SshWorker::stop() {
    Q_ASSERT(QThread::currentThread() == thread()); abortLookup();
    state_->stopping = true; if (state_->control) (*state_->control)->stop();
}
bool SshWorker::input(const QByteArray& bytes, quint64 generation, QString* error) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (generation != generation_ || state_->stopping || state_->retired || phase_ != Phase::Connected || !state_->control) return false;
    try { (*state_->control)->input(rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.constData()), size_t(bytes.size()))); return true; }
    catch (const rust::Error& failure) { if (error) *error = QString::fromUtf8(failure.what()); return false; }
}
bool SshWorker::resize(quint16 rows, quint16 columns, quint64 generation) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (generation != generation_ || state_->stopping || state_->retired || !state_->control) return false;
    try { (*state_->control)->resize(rows, columns); return true; } catch (const rust::Error&) { return false; }
}
bool SshWorker::answerHost(const QString& request, quint64 generation, bool allowed) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (generation != generation_ || state_->stopping || state_->retired || !state_->control) return false;
    const auto bytes = request.toUtf8(); return (*state_->control)->host_answer(rust::Str(bytes.constData(), size_t(bytes.size())), allowed);
}
void SshWorker::drain(const std::shared_ptr<State>& state, const QPointer<SshWorker>& owner) {
    if (state->retired || !state->control) return;
    auto event = (*state->control)->poll();
    const auto current = [&] { return owner && owner->state_ == state && owner->generation_ == state->generation; };
    if (current()) {
        const auto phase = static_cast<Phase>(event.phase);
        if (owner->phase_ != phase) {
            owner->phase_ = phase;
            if (phase == Phase::Failed || phase == Phase::Cancelled || phase == Phase::Disconnected) owner->abortLookup();
            emit owner->phaseChanged(phase, text(event.error), state->generation);
        }
        if (current() && !state->stopping && event.host_pending) {
            SshHostKey key;
            key.host = text(event.host_key.host); key.port = event.host_key.port; key.algorithm = text(event.host_key.algorithm);
            key.key = QByteArray(reinterpret_cast<const char*>(event.host_key.key.data()), qsizetype(event.host_key.key.size())); key.fingerprint = text(event.host_key.fingerprint);
            emit owner->hostKey(text(event.host_request), std::move(key), state->generation);
        }
        if (!state->stopping) {
            for (const auto& packet : event.packets) {
                if (!current() || state->stopping) break;
                emit owner->output(QByteArray(reinterpret_cast<const char*>(packet.bytes.data()), qsizetype(packet.bytes.size())), state->generation);
            }
        }
    }
    if (state->joined && event.ended) {
        state->retired = true; state->control.reset(); --liveWorkers;
        if (current()) emit owner->finished(state->generation);
        state->delivery->deleteLater();
    }
}
