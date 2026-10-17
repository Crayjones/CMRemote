#include "TerminalAppearanceService.h"
#include <QPointer>
#include <QThread>
#include <atomic>
#include <thread>

namespace { std::atomic<int> liveWorkers{0}; constexpr qsizetype maxBytes = 64 * 1024; }
struct TerminalAppearanceService::Reply {
    TerminalAppearance value;
    FileResult result;
    bool publish = false;
    Qt::HANDLE performedOn = nullptr;
};
struct TerminalAppearanceService::State {
    QString path;
    Options options;
    std::unique_ptr<AtomicFile> file;
    TerminalAppearance current;
    bool safe = false, valid = false;
    Reply load() {
        file = std::make_unique<AtomicFile>(path); file->setFault(options.fault);
        QByteArray bytes; bool exists = false; safe = !path.isEmpty() && file->load(bytes, exists, maxBytes);
        if (!safe) return {current, {false, "Terminal appearance file is unavailable or unsafe.", {}}};
        if (!exists) { valid = true; return {current, {true, {}, {}}}; }
        const auto decoded = TerminalAppearance::decode(bytes);
        if (!decoded) return {current, {false, "Terminal appearance file is invalid. Reset Defaults and Apply to replace the inspected file.", {}}};
        current = *decoded; valid = true; return {current, {true, {}, {}}, true};
    }
    Reply save(const TerminalAppearance& value, bool resetInvalid) {
        if (!value.valid() || !safe || (!valid && !resetInvalid)) return {current, {false, "Terminal appearance cannot be saved. Check the file or explicitly Reset Defaults.", {}}};
        if (valid && current == value) return {current, {true, {}, {}}};
        auto result = file->replace(value.encode(), maxBytes, "Terminal appearance");
        if (result.ok) { current = value; valid = true; }
        return {current, result, result.ok};
    }
};
TerminalAppearanceService::TerminalAppearanceService(QString path, QObject* parent) : TerminalAppearanceService(std::move(path), Options{}, parent) {}
TerminalAppearanceService::TerminalAppearanceService(QString path, Options options, QObject* parent) : QObject(parent), state_(std::make_shared<State>()) {
    state_->path = std::move(path); state_->options = options; setObjectName("terminalAppearanceService");
    workerThread_ = new QThread; workerThread_->setObjectName("terminalAppearanceWorker"); worker_ = new QObject; worker_->moveToThread(workerThread_);
    delivery_ = new QObject; const auto owner = QPointer<TerminalAppearanceService>(this); auto* delivery = delivery_; auto* workerThread = workerThread_;
    connect(workerThread, &QThread::finished, worker_, &QObject::deleteLater);
    connect(workerThread, &QThread::finished, delivery, [owner, delivery, workerThread] {
        // Match SSH service retirement: native thread-local cleanup is joined off GUI.
        std::thread([owner, delivery, workerThread] {
            workerThread->wait();
            QMetaObject::invokeMethod(delivery, [owner, delivery, workerThread] {
                --liveWorkers; workerThread->deleteLater();
                if (owner) {
                    owner->workerThread_ = nullptr; owner->worker_ = nullptr; owner->delivery_ = nullptr; owner->finished_ = true;
                    auto callbacks = std::move(owner->shutdownCompletions_); for (auto& callback : callbacks) callback();
                }
                delivery->deleteLater();
            }, Qt::QueuedConnection);
        }).detach();
    }, Qt::QueuedConnection);
    ++liveWorkers; workerThread_->start(); execute([](State& state) { return state.load(); });
}
TerminalAppearanceService::~TerminalAppearanceService() { shutdown(); }
int TerminalAppearanceService::activeWorkers() { return liveWorkers.load(); }
void TerminalAppearanceService::execute(std::function<Reply(State&)> operation) {
    Q_ASSERT(QThread::currentThread() == thread()); busy_ = true; emit changed();
    const auto owner = QPointer<TerminalAppearanceService>(this); const auto state = state_; auto* delivery = delivery_;
    QMetaObject::invokeMethod(worker_, [owner, state, delivery, operation = std::move(operation)] {
        auto reply = operation(*state); reply.performedOn = QThread::currentThreadId();
        QMetaObject::invokeMethod(delivery, [owner, reply = std::move(reply)] {
            if (!owner) return;
            owner->ready_ = true; owner->busy_ = false; owner->performedOn_ = reply.performedOn;
            owner->current_ = reply.value; owner->error_ = reply.result.error; owner->warning_ = reply.result.warning;
            if (reply.publish) emit owner->applied();
            emit owner->changed();
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}
void TerminalAppearanceService::apply(TerminalAppearance value, bool resetInvalid) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!ready_ || busy_ || shuttingDown_ || !warning_.isEmpty()) return;
    execute([value = std::move(value), resetInvalid](State& state) { return state.save(value, resetInvalid); });
}
void TerminalAppearanceService::acknowledgeWarning() { warning_.clear(); emit changed(); }
void TerminalAppearanceService::setFault(AtomicFile::Fault fault) {
    if (shuttingDown_) return;
    const auto state = state_; QMetaObject::invokeMethod(worker_, [state, fault] { state->options.fault = fault; if (state->file) state->file->setFault(fault); }, Qt::QueuedConnection);
}
void TerminalAppearanceService::shutdown(std::function<void()> complete) {
    if (finished_) { if (complete) QMetaObject::invokeMethod(this, std::move(complete), Qt::QueuedConnection); return; }
    if (complete) shutdownCompletions_.push_back(std::move(complete));
    if (shuttingDown_) return;
    shuttingDown_ = true; const auto state = state_;
    QMetaObject::invokeMethod(worker_, [state] { state->file.reset(); QThread::currentThread()->quit(); }, Qt::QueuedConnection);
}
