#include "DocumentContext.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <stdexcept>

namespace {
rust::Str view(const QByteArray& bytes) { return {bytes.constData(), size_t(bytes.size())}; }
QByteArray bytes(const rust::String& value) { return {value.data(), qsizetype(value.size())}; }
QString text(const rust::String& value) { return QString::fromUtf8(value.data(), qsizetype(value.size())); }
}
DocumentContext::DocumentContext(DocumentId id) : id_(id), core_(rm::detached_core()), selection_(DocumentRootReference{id}) {
    assertGui(); Q_ASSERT(id.valid()); status_.id = id;
}
void DocumentContext::assertGui() const { Q_ASSERT(QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread()); }
rm::Core& DocumentContext::core() { assertGui(); return *core_; }
const rm::Core& DocumentContext::core() const { assertGui(); return *core_; }
bool DocumentContext::canWrite() const {
    return loaded_ && status_.unlocked && !status_.busy && !pending() && !status_.repairRequired && status_.warning.isEmpty() && error_.isEmpty();
}
bool DocumentContext::canRepairIds() const {
    return loaded_ && status_.repairRequired && status_.unlocked &&
        !status_.busy && !pending() && status_.warning.isEmpty() && error_.isEmpty();
}
bool DocumentContext::canClose() const {
    const bool settled = !pending() || (lastCommitted_ && !error_.isEmpty());
    return settled && !status_.busy && status_.warning.isEmpty() && core_->session_count() == 0;
}
bool DocumentContext::updateStatus(const DocumentStatus& status) {
    assertGui(); if (status.id != id_ || status.generation < status_.generation) return false;
    if (status.generation != status_.generation || status.path != status_.path || status.unlocked != status_.unlocked) ++treeGeneration_;
    status_ = status; status_.metadataAvailable = loaded_; return true;
}
void DocumentContext::refresh() {
    ++treeGeneration_; profiles_.clear(); order_.clear();
    const auto snapshot = QJsonDocument::fromJson(bytes(core_->snapshot())).object();
    for (const auto& value : snapshot.value("profiles").toArray()) {
        const auto profile = value.toObject(); const auto id = profile.value("id").toString();
        profiles_.insert(id, profile); order_.append(id);
    }
    if (const auto* selected = std::get_if<ProfileReference>(&selection_); selected && !profiles_.contains(selected->profile)) selection_ = DocumentRootReference{id_};
    expanded_.intersect(QSet<QString>(profiles_.keyBegin(), profiles_.keyEnd()));
}
DocumentCandidate DocumentContext::prepareProfile(rm::ProfileChange change, const QByteArray& input) {
    assertGui(); if (!canWrite()) throw std::runtime_error("Connection file is locked, busy or awaiting recovery.");
    auto prepared = core_->prepare_profile(change, view(input)); reservation_ = text(prepared.token);
    return {id_, reservation_, text(prepared.id), bytes(prepared.json)};
}
DocumentCandidate DocumentContext::prepareDocument() {
    assertGui(); if (!canWrite()) throw std::runtime_error("Connection file is locked, busy or awaiting recovery.");
    auto prepared = core_->prepare_document(); reservation_ = text(prepared.token);
    return {id_, reservation_, {}, bytes(prepared.json)};
}
DocumentCandidate DocumentContext::prepareIdRepair() {
    assertGui(); if (!canRepairIds()) throw std::runtime_error("Missing-ID file is locked, busy or awaiting recovery.");
    auto prepared = core_->prepare_document(); reservation_ = text(prepared.token);
    return {id_, reservation_, {}, bytes(prepared.json)};
}
FileResult DocumentContext::adopt(const DocumentReply& reply) {
    assertGui();
    if (reply.status.id != id_) return {false, "Reply belongs to another connection file.", {}};
    if (reply.committed || !reply.reservation.isEmpty()) {
        if (!pending() || reservation_ != reply.reservation) return {false, "Reply does not own this file's save reservation.", {}};
        try {
            const auto token = reservation_.toUtf8();
            if (reply.committed) {
                core_->finish_document(view(token), view(reply.projection)); projection_ = bytes(core_->stored_document()); loaded_ = true;
            } else core_->finish_profile(view(token), false);
            reservation_.clear(); refresh();
        } catch (const std::exception&) {
            // A committed file cannot be described as aborted. Keep the owning
            // reservation for recovery and block new writes on this context.
            updateStatus(reply.status); error_ = "Cannot reconcile this file's terminal save. Reopen it after closing its saved tabs.";
            lastCommitted_ = reply.committed; outcome_ = {false, error_, reply.result.warning}; return outcome_;
        }
    } else if (reply.result.ok && !reply.canceled && !reply.projection.isEmpty()) {
        if (reply.status.generation < status_.generation) return {false, "Stale connection-file reply.", {}};
        try {
            if (loaded_) {
                auto validated = rm::detached_core(); validated->load_document(view(reply.projection));
                if (bytes(validated->stored_document()) != projection_) return {false, "Connection metadata changed without an owning save reservation.", {}};
            }
            if (!loaded_) { core_->load_document(view(reply.projection)); loaded_ = true; projection_ = bytes(core_->stored_document()); refresh(); }
        } catch (const std::exception&) { return {false, "Cannot load validated connection metadata.", {}}; }
    }
    updateStatus(reply.status); error_.clear(); outcome_ = reply.result; lastCommitted_ = reply.committed; return outcome_;
}
bool DocumentContext::select(DocumentRootReference root) {
    assertGui(); if (root.document != id_) return false; selection_ = root; return true;
}
bool DocumentContext::select(const ProfileReference& profile) {
    assertGui(); if (!profile.valid() || profile.document != id_ || !profiles_.contains(profile.profile)) return false;
    selection_ = profile; return true;
}
