#include "VncWorker.h"
#include "VncGraphics.h"
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
bool terminal(VncWorker::Phase phase) {
    return phase == VncWorker::Phase::Disconnected || phase == VncWorker::Phase::Cancelled || phase == VncWorker::Phase::Failed;
}
}
struct VncWorker::State {
    std::optional<rust::Box<rm::vnc::BackendControl>> control;
    VncGraphics graphics;
    QObject* delivery = nullptr;
    QThread* worker = nullptr;
    quint64 generation = 0;
    bool stopping = false, joined = false, retired = false;
    bool framePosted = false, cursorPosted = false;
};
VncWorker::VncWorker(SessionTarget target, rust::Box<rm::vnc::Authentication> authentication, quint64 generation, QObject* parent)
    : QObject(parent), state_(std::make_shared<State>()), generation_(generation) {
    setObjectName("vncWorker"); state_->generation = generation;
    auto* delivery = new QObject; delivery->setObjectName("vncWorkerDelivery"); state_->delivery = delivery;
    const auto owner = QPointer<VncWorker>(this); const auto state = state_; const auto weak = std::weak_ptr<State>(state);
    const auto sink = std::make_shared<rm::vnc::EventSink>([weak](const rm::vnc::GraphicsEvent& event) {
        // Synchronous native-worker delivery: no QObject or GUI framebuffer access.
        const auto state = weak.lock();
        if (!state) return false;
        const auto* bytes = reinterpret_cast<const uchar*>(event.pixels.data());
        switch (event.kind) {
        case 0: return state->graphics.geometry(event.width, event.height);
        case 1: return state->graphics.raw(event.x, event.y, event.width, event.height, bytes, event.pixels.size());
        case 2: return state->graphics.copy(event.x, event.y, event.width, event.height, event.src_x, event.src_y);
        case 3: return state->graphics.cursor(event.x, event.y, event.width, event.height, bytes, event.pixels.size());
        case 4: return state->graphics.commit();
        default: return false;
        }
    }, [weak, owner, delivery](quint64 value) {
        // The retained context survives pane destruction, native TLS cleanup and join.
        QMetaObject::invokeMethod(delivery, [weak, owner, value] {
            if (const auto state = weak.lock(); state && state->generation == value) drain(state, owner);
        }, Qt::QueuedConnection);
    });
    rm::vnc::BackendTarget snapshot;
    snapshot.host = owned(target.host); snapshot.port = target.port; snapshot.view_only = target.viewOnly;
    std::optional<rust::Box<rm::vnc::Backend>> backend;
    try { backend.emplace(rm::vnc::new_backend(std::move(snapshot), std::move(authentication), sink, generation)); }
    catch (...) { delete delivery; throw; }
    state->control.emplace((*backend)->control());
    auto* worker = QThread::create([backend = std::move(*backend)]() mutable { rm::vnc::run_backend(std::move(backend)); });
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
VncWorker::~VncWorker() { stop(); }
int VncWorker::activeWorkers() { return liveWorkers.load(); }
void VncWorker::abortLookup() { if (lookup_ >= 0) { QHostInfo::abortHostLookup(lookup_); lookup_ = -1; } }
void VncWorker::stop() {
    Q_ASSERT(QThread::currentThread() == thread()); abortLookup();
    state_->stopping = true; if (state_->control) (*state_->control)->stop();
}
bool VncWorker::key(quint32 keysym, bool down, quint64 generation, QString* error) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (generation != generation_ || state_->stopping || state_->retired || phase_ != Phase::Connected || !state_->control) return false;
    try { (*state_->control)->key(keysym, down); return true; }
    catch (const rust::Error& failure) { if (error) *error = QString::fromUtf8(failure.what()); return false; }
}
bool VncWorker::pointer(quint8 mask, quint16 x, quint16 y, quint64 generation, QString* error) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (generation != generation_ || state_->stopping || state_->retired || phase_ != Phase::Connected || !state_->control) return false;
    try { (*state_->control)->pointer(mask, x, y); return true; }
    catch (const rust::Error& failure) { if (error) *error = QString::fromUtf8(failure.what()); return false; }
}
bool VncWorker::password(Secret password, quint64 generation, QString* error) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (generation != generation_ || state_->stopping || state_->retired || phase_ != Phase::PasswordRequired || !state_->control) return false;
    rust::Vec<uint8_t> bytes; bytes.reserve(password.size());
    for (size_t i = 0; i < password.size(); ++i) bytes.push_back(password.data()[i]);
    password.clear();
    try { (*state_->control)->password(std::move(bytes)); return true; }
    catch (const rust::Error& failure) { if (error) *error = QString::fromUtf8(failure.what()); return false; }
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
    if (state->retired || !state->control) return;
    const auto event = (*state->control)->poll();
    const auto phase = static_cast<Phase>(event.phase);
    const auto current = [&] { return owner && owner->state_ == state && owner->generation_ == state->generation; };
    if (current()) {
        if (owner->phase_ != phase) {
            owner->phase_ = phase;
            if (terminal(phase)) owner->abortLookup();
            emit owner->phaseChanged(phase, text(event.error), state->generation);
        }
        if (current() && !state->stopping && !state->framePosted && state->graphics.hasUpdate()) {
            state->framePosted = true; emit owner->frameAvailable(state->generation);
        }
        if (current() && !state->stopping && !state->cursorPosted && state->graphics.hasCursor()) {
            state->cursorPosted = true; emit owner->cursorAvailable(state->generation);
        }
    }
    if (state->joined && terminal(phase)) {
        // With the producer joined no further snapshots can arrive. Discard any
        // final unconsumed mailboxes before releasing the retained delivery context.
        state->graphics.takeUpdate(); state->graphics.takeCursor();
        state->retired = true; state->control.reset(); --liveWorkers;
        if (current()) emit owner->finished(state->generation);
        state->delivery->deleteLater();
    }
}
