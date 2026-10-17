#pragma once
#include "DocumentService.h"
#include "remotemanager-core/src/bridge.rs.h"
#include <QJsonObject>
#include <QSet>
#include <variant>

struct DocumentCandidate {
    DocumentId document;
    QString reservation, profile;
    QByteArray projection;
};
// GUI-owned Core and view state. Worker replies reconcile this owner, never the
// currently selected root. Sessions stay in this Core across file-only commits.
class DocumentContext {
public:
    explicit DocumentContext(DocumentId id);
    DocumentId id() const { return id_; }
    const DocumentStatus& status() const { return status_; }
    const FileResult& outcome() const { return outcome_; }
    bool lastCommitted() const { return lastCommitted_; }
    const QHash<QString, QJsonObject>& profiles() const { return profiles_; }
    // Profile IDs in saved file order; siblings appear in display order.
    const QStringList& order() const { return order_; }
    const QByteArray& projection() const { return projection_; }
    rm::Core& core();
    const rm::Core& core() const;
    bool updateStatus(const DocumentStatus& status);
    FileResult adopt(const DocumentReply& reply);
    DocumentCandidate prepareProfile(rm::ProfileChange change, const QByteArray& input);
    DocumentCandidate prepareDocument();
    DocumentCandidate prepareIdRepair();
    bool pending() const { return !reservation_.isEmpty(); }
    quint64 treeGeneration() const { return treeGeneration_; }
    bool canWrite() const;
    bool canRepairIds() const;
    bool canClose() const;
    bool select(DocumentRootReference root);
    bool select(const ProfileReference& profile);
    const std::variant<DocumentRootReference, ProfileReference>& selection() const { return selection_; }
    QSet<QString>& expanded() { return expanded_; }
    bool rootExpanded() const { return rootExpanded_; }
    void setRootExpanded(bool expanded) { rootExpanded_ = expanded; }
    void acknowledgeWarning() { status_.warning.clear(); }
    QString error() const { return error_; }
private:
    void assertGui() const;
    void refresh();
    DocumentId id_;
    rust::Box<rm::Core> core_;
    DocumentStatus status_;
    QByteArray projection_;
    QHash<QString, QJsonObject> profiles_;
    QStringList order_;
    std::variant<DocumentRootReference, ProfileReference> selection_;
    QSet<QString> expanded_;
    QString reservation_, error_;
    bool loaded_ = false, lastCommitted_ = false, rootExpanded_ = true;
    FileResult outcome_;
    quint64 treeGeneration_ = 0;
};
