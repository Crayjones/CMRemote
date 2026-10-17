#include "DocumentService.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSaveFile>
#include <openssl/crypto.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#elif defined(Q_OS_WIN)
#include <QFile>
#include <io.h>
#include <windows.h>
#endif

namespace {
FileResult error(const char* text) { return {false, QString::fromLatin1(text), {}}; }
bool defaultMaster(const Secret& master) {
    return master.size() == 4 && QByteArray::fromRawData(reinterpret_cast<const char*>(master.data()), 4) == "mR3m";
}
bool sameFile(const QString& a, const QString& b) {
    if (QDir::cleanPath(a) == QDir::cleanPath(b)) return true;
#ifdef Q_OS_UNIX
    struct stat first{}, second{};
    return ::stat(QFile::encodeName(a).constData(), &first) == 0 && ::stat(QFile::encodeName(b).constData(), &second) == 0 &&
        first.st_dev == second.st_dev && first.st_ino == second.st_ino;
#elif defined(Q_OS_WIN)
    QFile first(a), second(b); BY_HANDLE_FILE_INFORMATION left{}, right{};
    return first.open(QIODevice::ReadOnly) && second.open(QIODevice::ReadOnly) &&
        GetFileInformationByHandle(reinterpret_cast<HANDLE>(_get_osfhandle(first.handle())), &left) &&
        GetFileInformationByHandle(reinterpret_cast<HANDLE>(_get_osfhandle(second.handle())), &right) &&
        left.dwVolumeSerialNumber == right.dwVolumeSerialNumber && left.nFileIndexHigh == right.nFileIndexHigh && left.nFileIndexLow == right.nFileIndexLow;
#else
    return false;
#endif
}
void clearReply(DocumentReply& reply) {
    reply.password.reset(); reply.canceled = true; reply.status.unlocked = false; reply.status.compatibilityMaster = false;
    if (!reply.committed && !reply.closed) {
        reply.projection.fill('\0'); reply.projection.clear(); reply.destination.reset();
        reply.result = error("Document operation canceled.");
    }
}
}
struct DocumentService::Slot {
    DocumentStatus status;
    QTimer* idle = nullptr;
    bool opening = true;
    std::atomic<quint64> generation{0};
    std::mutex commit;
    std::vector<std::weak_ptr<DocumentReply>> replies;
    void invalidate() {
        std::lock_guard<std::mutex> guard(commit); ++generation;
        for (auto& weak : replies) if (auto reply = weak.lock()) clearReply(*reply);
        replies.clear();
    }
};
struct DocumentService::State {
    struct Destination {
        DestinationInspection inspection;
        AtomicFile file;
        explicit Destination(AtomicFile inspected, bool exists) : file(std::move(inspected)) {
            inspection = {QUuid::createUuid().toString(QUuid::WithoutBraces), file.path(), file.revision(), exists};
        }
    };
    struct Document {
        explicit Document(QString path) : store(std::move(path)) {}
        DocumentStore store;
        std::optional<Secret> master;
        std::optional<Destination> destination;
        AtomicFile::Fault fault = AtomicFile::Fault::None;
        AtomicFile::CommitGate userGate;
        bool writable() const { return master.has_value(); }
        void lock() { master.reset(); destination.reset(); }
    };
    QHash<DocumentId, std::shared_ptr<Document>> documents;
    Document* find(DocumentReply& reply) {
        const auto found = documents.value(reply.status.id);
        if (!found) reply.result = error("Connection file is not open.");
        return found.get();
    }
    std::optional<DocumentId> alias(DocumentId owner, const QString& path, bool includeOwner = false) const {
        for (auto i = documents.cbegin(); i != documents.cend(); ++i)
            if ((includeOwner || i.key() != owner) && sameFile(path, i.value()->store.path())) return i.key();
        return {};
    }
    DocumentStatus status(DocumentId id) const {
        DocumentStatus result; result.id = id;
        const auto doc = documents.value(id); if (!doc) return result;
        result.path = doc->store.path(); result.revision = doc->store.revision(); result.exists = doc->store.exists();
        result.unlocked = doc->writable();
        result.compatibilityMaster = doc->master && defaultMaster(*doc->master);
        if (const auto* xml = doc->store.xml()) {
            result.fullEncryption = xml->fullEncryption(); result.metadataAvailable = !xml->needsUnlock();
            result.passwordProtected = xml->passwordProtected();
            result.repairRequired = xml->requiresIdRepair(); result.reviewRequired = xml->credentialReviewRequired();
            result.passwords = xml->passwordStates();
        }
        return result;
    }
    AtomicFile::CommitGate gate(DocumentId id, const std::shared_ptr<Slot>& slot, quint64 generation, QString path) {
        const auto doc = documents.value(id); const auto user = doc ? doc->userGate : AtomicFile::CommitGate();
        return [this, id, weak = std::weak_ptr<Slot>(slot), generation, path = std::move(path), user](const auto& commit) {
            const auto guarded = [&] {
                const auto slot = weak.lock(); if (!slot) return false;
                std::lock_guard<std::mutex> guard(slot->commit);
                const auto doc = documents.value(id);
                return slot->generation.load() == generation && doc && !alias(id, path) && !alias(id, doc->store.path()) && commit();
            };
            return user ? user(guarded) : guarded();
        };
    }
    void projection(Document& doc, DocumentReply& reply, const ConnectionXml::Canceled& canceled) {
        if (!doc.store.xml() || doc.store.xml()->needsUnlock()) return;
        const auto json = doc.store.xml()->projection(canceled);
        if (json) reply.projection = *json;
        else reply.result = error("Cannot validate connection metadata.");
    }
};
DocumentService::DocumentService(QObject* parent) : QObject(parent), state_(std::make_shared<State>()) {
    setObjectName("connectionFiles"); workerThread_.setObjectName("connectionFilesWorker");
    worker_ = new QObject; worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater); workerThread_.start();
}
DocumentService::~DocumentService() {
    for (auto& slot : slots_) { slot->invalidate(); slot->idle->stop(); }
    auto state = state_;
    QMetaObject::invokeMethod(worker_, [state] { state->documents.clear(); QThread::currentThread()->quit(); }, Qt::QueuedConnection);
    workerThread_.wait();
}
std::optional<DocumentStatus> DocumentService::status(DocumentId id) const {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto slot = slots_.value(id); return slot ? std::optional<DocumentStatus>(slot->status) : std::nullopt;
}
void DocumentService::lock(DocumentId id) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto slot = slots_.value(id); if (!slot) return;
    slot->invalidate(); slot->idle->stop(); slot->status.unlocked = false; slot->status.compatibilityMaster = false; slot->status.generation = slot->generation.load();
    auto state = state_; QMetaObject::invokeMethod(worker_, [state, id] {
        if (const auto doc = state->documents.value(id)) doc->lock();
    }, Qt::QueuedConnection);
    QPointer<DocumentService> owner(this); emit locked(id); if (owner) emit stateChanged(id);
}
void DocumentService::acknowledgeWarning(DocumentId id) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (const auto slot = slots_.value(id)) { slot->status.warning.clear(); emit stateChanged(id); }
}
void DocumentService::setIdleInterval(DocumentId id, int milliseconds) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (const auto slot = slots_.value(id)) {
        slot->idle->setInterval(qMax(1, milliseconds)); if (slot->status.unlocked && !slot->status.busy) slot->idle->start();
    }
}
void DocumentService::execute(DocumentId id, QString reservation, Operation operation, Completion complete) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto slot = slots_.value(id);
    if (!slot || slot->status.busy || !slot->status.warning.isEmpty()) {
        DocumentReply reply; reply.status = slot ? slot->status : DocumentStatus(); reply.status.id = id; reply.reservation = std::move(reservation);
        reply.result = error(!slot ? "Connection file is not open." : slot->status.busy ? "Wait for this file's storage operation to finish." : "Acknowledge this file's durability warning before continuing.");
        auto owned = std::make_shared<DocumentReply>(std::move(reply));
        QMetaObject::invokeMethod(this, [owned, complete = std::move(complete)]() mutable { complete(std::move(*owned)); }, Qt::QueuedConnection); return;
    }
    slot->status.busy = true; slot->idle->stop();
    const auto generation = slot->generation.load(); const bool opening = slot->opening;
    auto state = state_; QPointer<DocumentService> owner(this);
    QMetaObject::invokeMethod(worker_, [state, slot, id, generation, opening, owner, reservation = std::move(reservation), operation = std::move(operation), complete = std::move(complete)]() mutable {
        auto reply = std::make_shared<DocumentReply>(); reply->status.id = id; reply->reservation = std::move(reservation);
        const auto canceled = [slot, generation] { return slot->generation.load() != generation; };
        if (const auto doc = state->documents.value(id)) doc->store.setCommitGate(state->gate(id, slot, generation, doc->store.path()));
        if (!canceled()) operation(*state, *reply, canceled); else reply->result = error("Document operation canceled before replacement.");
        reply->status = state->status(id); const auto* performedOn = QThread::currentThread();
        {
            std::lock_guard<std::mutex> guard(slot->commit);
            if (canceled()) {
                clearReply(*reply);
                if (const auto doc = state->documents.value(id)) doc->lock();
                if (opening && !reply->committed) state->documents.remove(id);
            }
            slot->replies.erase(std::remove_if(slot->replies.begin(), slot->replies.end(), [](const auto& weak) { return weak.expired(); }), slot->replies.end());
            slot->replies.push_back(reply);
        }
        if (!owner) return;
        QMetaObject::invokeMethod(owner, [state, slot, id, generation, opening, owner, reply, performedOn, complete = std::move(complete)]() mutable {
            if (!owner) return;
            const auto previous = slot->status;
            if (reply->status.path.isEmpty()) reply->status.path = previous.path;
            {
                std::lock_guard<std::mutex> guard(slot->commit);
                if (slot->generation.load() != generation) {
                    clearReply(*reply);
                    if (!reply->committed) {
                        reply->status = slot->status; reply->status.busy = false;
                    }
                }
                reply->status.generation = slot->generation.load(); reply->status.busy = false;
                reply->status.unlocked = reply->status.unlocked && !reply->canceled;
                reply->status.warning = reply->result.warning;
                slot->status = reply->status; slot->opening = false;
            }
            const bool remove = reply->closed || (opening && !reply->result.ok && !reply->committed);
            if (remove) {
                owner->slots_.remove(id); slot->idle->deleteLater();
                QMetaObject::invokeMethod(owner->worker_, [state, id] { state->documents.remove(id); }, Qt::QueuedConnection);
            } else if (slot->status.unlocked) slot->idle->start();
            emit owner->operationThread(id, const_cast<QThread*>(performedOn)); if (!owner) return;
            emit owner->stateChanged(id); if (!owner) return;
            DocumentReply delivered;
            {
                std::lock_guard<std::mutex> guard(slot->commit);
                if (slot->generation.load() != generation) {
                    clearReply(*reply);
                    if (!reply->committed) {
                        slot->status.metadataAvailable = previous.metadataAvailable;
                        slot->status.repairRequired = previous.repairRequired;
                        slot->status.reviewRequired = previous.reviewRequired;
                    }
                }
                reply->status = slot->status;
                delivered = std::move(*reply);
            }
            complete(std::move(delivered));
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
    emit stateChanged(id);
}
DocumentId DocumentService::createSlot(QString path) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto id = DocumentId::create(); auto slot = std::make_shared<Slot>(); slot->status.id = id; slot->status.path = std::move(path);
    slot->idle = new QTimer(this); slot->idle->setSingleShot(true); slot->idle->setInterval(15 * 60 * 1000);
    connect(slot->idle, &QTimer::timeout, this, [this, id] { lock(id); }); slots_.insert(id, slot); return id;
}
DocumentId DocumentService::open(QString path, Completion complete, bool createNew) {
    const auto id = createSlot(path);
    execute(id, {}, [path = std::move(path), createNew](State& state, DocumentReply& reply, const auto& canceled) {
        auto doc = std::make_shared<State::Document>(path);
        const auto read = doc->store.loadXml(nullptr, canceled); reply.result = read.result; if (!read.result.ok) return;
        if (createNew == read.exists) { reply.result = error(createNew ? "New requires an unused destination." : "Connection file does not exist."); return; }
        if (const auto other = state.alias(reply.status.id, doc->store.path())) {
            reply.alreadyOpen = other; reply.result = error("This physical connection file is already open."); return;
        }
        state.documents.insert(reply.status.id, doc);
        Secret initial(QByteArray("mR3m"));
        if (doc->store.authenticateXml(initial, canceled).ok) doc->master = std::move(initial);
        state.projection(*doc, reply, canceled);
        if (!reply.result.ok) state.documents.remove(reply.status.id);
    }, std::move(complete)); return id;
}
void DocumentService::unlock(DocumentId id, Secret master, Completion complete) {
    auto owned = std::make_shared<Secret>(std::move(master));
    execute(id, {}, [owned](State& state, DocumentReply& reply, const auto& canceled) {
        auto* doc = state.find(reply); if (!doc) return;
        reply.result = doc->store.authenticateXml(*owned, canceled);
        if (reply.result.ok) {
            doc->master = std::move(*owned); state.projection(*doc, reply, canceled);
        }
        owned->clear();
    }, std::move(complete));
}
void DocumentService::reviewPasswords(DocumentId id, std::vector<PasswordAuthorization> reviewed, Completion complete) {
    execute(id, {}, [reviewed = std::move(reviewed)](State& state, DocumentReply& reply, const auto& canceled) {
        auto* doc = state.find(reply); if (!doc) return;
        if (!doc->writable()) { reply.result = error("Unlock this file before reviewing reusable passwords."); return; }
        reply.result = doc->store.reviewXmlPasswords(*doc->master, reviewed, canceled);
    }, std::move(complete));
}
void DocumentService::repairIds(DocumentId id, std::optional<Secret> replacement, QString reservation, Completion complete) {
    auto owned = std::make_shared<std::optional<Secret>>(std::move(replacement));
    execute(id, std::move(reservation), [owned](State& state, DocumentReply& reply, const auto& canceled) {
        auto* doc = state.find(reply); if (!doc) return;
        if (!doc->master || !doc->store.xml()->requiresIdRepair() ||
            (*owned && !ConnectionCrypto::validNewMaster(**owned))) {
            reply.result = error("Unlock the missing-ID file before repair."); return;
        }
        const auto projection = doc->store.xml()->projection(canceled);
        if (!projection) { reply.result = error("Connection metadata is not available for ID repair."); return; }
        reply.result = doc->store.updateXml(*doc->master, [&](ConnectionXml& xml) {
            return xml.authorizeIdRepair() && (!*owned || xml.changeMaster(*doc->master, **owned, canceled));
        }, *owned ? &**owned : nullptr, canceled);
        reply.committed = reply.result.ok;
        if (reply.committed) {
            reply.projection = *projection;
            if (*owned) { doc->master = std::move(**owned); }
        }
        owned->reset();
    }, std::move(complete));
}
void DocumentService::save(DocumentId id, QByteArray projection, QString reservation, Completion complete) {
    execute(id, std::move(reservation), [projection = std::move(projection)](State& state, DocumentReply& reply, const auto& canceled) {
        auto* doc = state.find(reply); if (!doc) return;
        if (!doc->writable()) { reply.result = error("Unlock this file before saving."); return; }
        std::optional<QByteArray> normalized;
        reply.result = doc->store.updateXml(*doc->master, [&](ConnectionXml& xml) {
            // A host or account changed here keeps its saved password; one changed outside the app does not.
            const auto bound = xml.boundRecords(canceled);
            normalized = xml.patch(*doc->master, projection, canceled); if (!normalized) return false;
            return xml.followAccountEdits(*doc->master, bound, canceled);
        }, nullptr, canceled);
        reply.committed = reply.result.ok;
        if (reply.committed) reply.projection = std::move(*normalized);
    }, std::move(complete));
}
void DocumentService::editPassword(ProfileReference profile, QByteArray projection, PasswordEdit edit,
                                  std::vector<PasswordAuthorization> reviewed, std::optional<Secret> replacement,
                                  QString reservation, Completion complete) {
    auto owned = std::make_shared<PasswordEdit>(std::move(edit));
    auto master = std::make_shared<std::optional<Secret>>(std::move(replacement));
    execute(profile.document, std::move(reservation), [profile = std::move(profile), projection = std::move(projection), reviewed = std::move(reviewed), owned, master](State& state, DocumentReply& reply, const auto& canceled) {
        auto* doc = state.find(reply); if (!doc) return;
        if (!profile.valid() || !doc->writable() || (*master && !ConnectionCrypto::validNewMaster(**master))) {
            reply.result = error("Unlock this file before saving a password."); return;
        }
        if (owned->kind == PasswordEdit::Kind::Set && (owned->value.size() == 0 || owned->value.size() > 4096 || !ConnectionCrypto::validUtf8(owned->value))) {
            reply.result = error("Enter a nonempty password of at most 4096 UTF-8 bytes, or use Clear password."); return;
        }
        std::optional<QByteArray> normalized;
        reply.result = doc->store.updateXml(*doc->master, [&](ConnectionXml& xml) {
            const Secret& key = *master ? **master : *doc->master;
            if (*master && !xml.changeMaster(*doc->master, key, canceled)) return false;
            normalized = xml.patch(key, projection, canceled); if (!normalized) return false;
            const Secret empty;
            const bool changed = owned->kind == PasswordEdit::Kind::Inheritance
                ? xml.setPasswordInheritance(key, profile.profile, owned->inherited, canceled)
                : xml.setPassword(key, profile.profile, owned->kind == PasswordEdit::Kind::Set ? owned->value : empty, canceled);
            if (!changed) return false;
            // Consent carries only committed endpoint/account data. The codec
            // independently resolves the actual source and ciphertext to bind.
            return xml.authorizePasswords(key, profile.profile, reviewed, canceled);
        }, *master ? &**master : nullptr, canceled);
        reply.committed = reply.result.ok;
        if (reply.committed) {
            reply.projection = std::move(*normalized);
            if (*master) { doc->master = std::move(**master); }
        }
        owned->value.clear(); master->reset();
    }, std::move(complete));
}
void DocumentService::changeMaster(DocumentId id, Secret replacement, QString reservation, Completion complete) {
    auto owned = std::make_shared<Secret>(std::move(replacement));
    execute(id, std::move(reservation), [owned](State& state, DocumentReply& reply, const auto& canceled) {
        auto* doc = state.find(reply); if (!doc) return;
        if (!doc->master || !ConnectionCrypto::validNewMaster(*owned)) { reply.result = error("Unlock this file and supply a confirmed nondefault master of 1–4096 compatible single-byte characters."); return; }
        // Capture the validated projection before replacement. Post-commit cancel
        // must not discard the metadata needed to finalize the owning GUI Core.
        const auto projection = doc->store.xml()->projection(canceled); if (!projection) { reply.result = error("Connection metadata is not available."); return; }
        reply.result = doc->store.updateXml(*doc->master, [&](ConnectionXml& xml) { return xml.changeMaster(*doc->master, *owned, canceled); }, owned.get(), canceled);
        reply.committed = reply.result.ok;
        if (reply.committed) { doc->master = std::move(*owned); reply.projection = *projection; }
        owned->clear();
    }, std::move(complete));
}
void DocumentService::resetMaster(DocumentId id, QString reservation, Completion complete) {
    execute(id, std::move(reservation), [](State& state, DocumentReply& reply, const auto& canceled) {
        auto* doc = state.find(reply); if (!doc) return;
        if (!doc->writable()) { reply.result = error("Unlock this file before removing its custom master."); return; }
        const auto projection = doc->store.xml()->projection(canceled);
        if (!projection) { reply.result = error("Connection metadata is not available."); return; }
        Secret replacement(QByteArray("mR3m"));
        reply.result = doc->store.updateXml(*doc->master, [&](ConnectionXml& xml) { return xml.resetMaster(*doc->master, canceled); }, &replacement, canceled);
        reply.committed = reply.result.ok;
        if (reply.committed) { doc->master = std::move(replacement); reply.projection = *projection; }
    }, std::move(complete));
}
void DocumentService::protect(DocumentId id, bool enabled, QString reservation, Completion complete) {
    execute(id, std::move(reservation), [enabled](State& state, DocumentReply& reply, const auto& canceled) {
        auto* doc = state.find(reply); if (!doc) return;
        if (!doc->writable()) { reply.result = error("Unlock this file before changing its protection."); return; }
        const auto projection = doc->store.xml()->projection(canceled); if (!projection) { reply.result = error("Connection metadata is not available."); return; }
        reply.result = doc->store.updateXml(*doc->master, [&](ConnectionXml& xml) { return xml.setFullEncryption(*doc->master, enabled, canceled); }, nullptr, canceled);
        reply.committed = reply.result.ok; if (reply.committed) reply.projection = *projection;
    }, std::move(complete));
}
void DocumentService::protect(DocumentId id, Secret replacement, QString reservation, Completion complete) {
    auto owned = std::make_shared<Secret>(std::move(replacement));
    execute(id, std::move(reservation), [owned](State& state, DocumentReply& reply, const auto& canceled) {
        auto* doc = state.find(reply); if (!doc) return;
        if (!doc->writable() || !ConnectionCrypto::validNewMaster(*owned)) { reply.result = error("Unlock this file and confirm a nondefault master of 1–4096 compatible single-byte characters."); return; }
        const auto projection = doc->store.xml()->projection(canceled); if (!projection) { reply.result = error("Connection metadata is not available."); return; }
        reply.result = doc->store.updateXml(*doc->master, [&](ConnectionXml& xml) {
            return xml.changeMaster(*doc->master, *owned, canceled) && xml.setFullEncryption(*owned, true, canceled);
        }, owned.get(), canceled);
        reply.committed = reply.result.ok;
        if (reply.committed) { doc->master = std::move(*owned); reply.projection = *projection; }
        owned->clear();
    }, std::move(complete));
}
void DocumentService::lookup(ProfileReference profile, SessionTarget target, Completion complete) {
    execute(profile.document, {}, [profile = std::move(profile), target = std::move(target)](State& state, DocumentReply& reply, const auto& canceled) {
        auto* doc = state.find(reply); if (!doc) return;
        if (!profile.valid() || !doc->writable() || !doc->store.xmlCurrent()) { reply.result = error("Unlock the unchanged owning file before requesting its password."); return; }
        reply.password = doc->store.xml()->password(*doc->master, profile.profile, target, canceled);
        reply.result.ok = doc->store.xmlCurrent() && !canceled();
        if (!reply.result.ok) { reply.password.reset(); reply.result = error("Connection file changed or password request was canceled."); }
    }, std::move(complete));
}
void DocumentService::lookupFolder(ProfileReference folder, Completion complete) {
    execute(folder.document, {}, [folder = std::move(folder)](State& state, DocumentReply& reply, const auto& canceled) {
        auto* doc = state.find(reply); if (!doc) return;
        if (!folder.valid() || !doc->writable() || !doc->store.xmlCurrent()) { reply.result = error("Unlock the unchanged owning file before requesting its password."); return; }
        reply.password = doc->store.xml()->folderPassword(*doc->master, folder.profile, canceled);
        reply.result.ok = doc->store.xmlCurrent() && !canceled();
        if (!reply.result.ok) { reply.password.reset(); reply.result = error("Connection file changed or password request was canceled."); }
    }, std::move(complete));
}
void DocumentService::inspectDestination(DocumentId id, QString path, Completion complete) {
    execute(id, {}, [path = std::move(path)](State& state, DocumentReply& reply, const auto& canceled) {
        auto* doc = state.find(reply); if (!doc) return;
        doc->destination.reset();
        if (!doc->writable() || !doc->store.xmlCurrent()) { reply.result = error("Unlock the unchanged source file before Save As."); return; }
        AtomicFile destination(path); QByteArray bytes; bool exists = false;
        if (!destination.load(bytes, exists, ConnectionXml::MaxFile) || canceled()) { reply.result = error("Cannot inspect Save As destination: unsafe path, size limit, read failure or canceled operation."); return; }
        bytes.fill('\0');
        if (const auto alias = state.alias(reply.status.id, destination.path(), true)) {
            reply.alreadyOpen = alias; reply.result = error("Save As destination aliases a loaded connection file."); return;
        }
        doc->destination.emplace(std::move(destination), exists); reply.destination = doc->destination->inspection; reply.result.ok = true;
    }, std::move(complete));
}
void DocumentService::saveAs(DocumentId id, QString ticket, bool overwriteConfirmed, QString reservation, Completion complete) {
    const auto slot = slots_.value(id); const auto generation = slot ? slot->generation.load() : 0;
    execute(id, std::move(reservation), [id, slot, generation, ticket = std::move(ticket), overwriteConfirmed](State& state, DocumentReply& reply, const auto& canceled) {
        auto* doc = state.find(reply); if (!doc) return;
        if (!doc->writable() || !doc->destination || doc->destination->inspection.ticket != ticket) { reply.result = error("Unlock this file and inspect a new Save As destination."); return; }
        if (doc->destination->inspection.exists && !overwriteConfirmed) { reply.result = error("Confirm replacement of the inspected destination before Save As."); return; }
        const auto path = doc->destination->file.path();
        if (const auto alias = state.alias(id, path, true)) { reply.alreadyOpen = alias; reply.result = error("Save As destination aliases a loaded connection file."); return; }
        const auto projection = doc->store.xml()->projection(canceled); if (!projection) { reply.result = error("Connection metadata is not available."); return; }
        auto destination = doc->destination->file; destination.setFault(doc->fault); destination.setCommitGate(state.gate(id, slot, generation, path));
        reply.result = doc->store.saveXmlAs(std::move(destination), *doc->master, canceled);
        reply.committed = reply.result.ok;
        if (reply.committed) { reply.projection = *projection; doc->destination.reset(); }
    }, std::move(complete));
}
void DocumentService::importProfiles(DocumentId id, QByteArray projection, std::shared_ptr<ImportedPasswords> passwords, QStringList consumers,
                                     QString reservation, Completion complete) {
    execute(id, std::move(reservation), [projection = std::move(projection), passwords, consumers = std::move(consumers)](State& state, DocumentReply& reply, const auto& canceled) {
        auto* doc = state.find(reply); if (!doc) return;
        if (!doc->writable()) { reply.result = error("Unlock this file before importing."); return; }
        std::optional<QByteArray> normalized;
        reply.result = doc->store.updateXml(*doc->master, [&](ConnectionXml& xml) {
            normalized = xml.patch(*doc->master, projection, canceled); if (!normalized) return false;
            for (const auto& owner : passwords->inherited) if (!xml.setPasswordInheritance(*doc->master, owner, true, canceled)) return false;
            for (const auto& [owner, value] : passwords->local) if (!xml.setPassword(*doc->master, owner, value, canceled)) return false;
            // Choosing the file to import is consent to reuse its passwords for the imported accounts.
            return xml.authorizeConsumers(*doc->master, consumers, canceled);
        }, nullptr, canceled);
        reply.committed = reply.result.ok;
        if (reply.committed) reply.projection = std::move(*normalized);
        passwords->local.clear();
    }, std::move(complete));
}
void DocumentService::exportFile(DocumentId id, QString path, ExportOptions options, Completion complete) {
    execute(id, {}, [path = std::move(path), options = std::move(options)](State& state, DocumentReply& reply, const auto& canceled) {
        auto* doc = state.find(reply); if (!doc) return;
        if (!doc->writable() || !doc->store.xml()) { reply.result = error("Unlock this file before exporting."); return; }
        if (state.alias(reply.status.id, path, true)) { reply.result = error("Choose a file other than an open connection file."); return; }
        auto bytes = doc->store.xml()->exportFile(*doc->master, options, canceled);
        if (!bytes) { reply.result = error("Cannot export this connection file."); return; }
        QSaveFile file(path);
        const bool written = file.open(QIODevice::WriteOnly) && file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) &&
            file.write(*bytes) == bytes->size() && !canceled() && file.commit();
        OPENSSL_cleanse(bytes->data(), size_t(bytes->size()));
        reply.result = written ? FileResult{true, {}, {}} : error("Cannot write the export file.");
    }, std::move(complete));
}
void DocumentService::close(DocumentId id, Completion complete) {
    const auto slot = slots_.value(id);
    if (slot && !slot->status.busy && slot->status.warning.isEmpty()) {
        slot->invalidate(); slot->idle->stop(); slot->status.unlocked = false; slot->status.compatibilityMaster = false; slot->status.generation = slot->generation.load();
    }
    execute(id, {}, [](State& state, DocumentReply& reply, const auto&) {
        state.documents.remove(reply.status.id); reply.closed = true; reply.result.ok = true;
    }, std::move(complete));
}
void DocumentService::setFault(DocumentId id, AtomicFile::Fault fault, Completion complete) {
    execute(id, {}, [fault](State& state, DocumentReply& reply, const auto&) {
        if (auto* doc = state.find(reply)) { doc->fault = fault; doc->store.setFault(fault); reply.result.ok = true; }
    }, std::move(complete));
}
void DocumentService::setCommitGate(DocumentId id, AtomicFile::CommitGate gate, Completion complete) {
    execute(id, {}, [gate = std::move(gate)](State& state, DocumentReply& reply, const auto&) {
        if (auto* doc = state.find(reply)) { doc->userGate = gate; reply.result.ok = true; }
    }, std::move(complete));
}
