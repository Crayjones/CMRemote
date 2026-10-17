#pragma once
#include "DocumentIdentity.h"
#include "DocumentStore.h"
#include <QHash>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <memory>

struct DocumentStatus {
    DocumentId id;
    QString path;
    QByteArray revision;
    quint64 generation = 0;
    bool exists = false, metadataAvailable = false, fullEncryption = false;
    bool unlocked = false, repairRequired = false, reviewRequired = false;
    std::optional<bool> passwordProtected;
    bool busy = false, compatibilityMaster = false;
    QHash<QString, PasswordState> passwords;
    QString warning;
};
struct DestinationInspection {
    QString ticket, path;
    QByteArray revision;
    bool exists = false;
};
// Move-only replies travel in owned queued functors, never Qt roles or signals.
struct DocumentReply {
    FileResult result;
    DocumentStatus status;
    QByteArray projection;
    QString reservation;
    std::optional<DocumentId> alreadyOpen;
    std::optional<DestinationInspection> destination;
    std::optional<Secret> password;
    bool committed = false, canceled = false, closed = false;
};

// One serialized worker owns every store/master. GUI slots contain only status,
// independent cancellation gates and terminal reply ledgers; no Core is borrowed.
class DocumentService final : public QObject {
    Q_OBJECT
public:
    using Completion = std::function<void(DocumentReply)>;
    explicit DocumentService(QObject* parent = nullptr);
    ~DocumentService() override;
    DocumentId open(QString path, Completion complete, bool createNew = false);
    void unlock(DocumentId id, Secret master, Completion complete);
    void reviewPasswords(DocumentId id, std::vector<PasswordAuthorization> reviewed, Completion complete);
    void repairIds(DocumentId id, std::optional<Secret> replacement, QString reservation, Completion complete);
    void save(DocumentId id, QByteArray projection, QString reservation, Completion complete);
    void changeMaster(DocumentId id, Secret replacement, QString reservation, Completion complete);
    void resetMaster(DocumentId id, QString reservation, Completion complete);
    void editPassword(ProfileReference profile, QByteArray projection, PasswordEdit edit,
                      std::vector<PasswordAuthorization> reviewed, std::optional<Secret> replacement,
                      QString reservation, Completion complete);
    void protect(DocumentId id, bool enabled, QString reservation, Completion complete);
    void protect(DocumentId id, Secret replacement, QString reservation, Completion complete);
    void lookup(ProfileReference profile, SessionTarget target, Completion complete);
    void lookupFolder(ProfileReference folder, Completion complete);
    void inspectDestination(DocumentId id, QString path, Completion complete);
    void saveAs(DocumentId id, QString ticket, bool overwriteConfirmed, QString reservation, Completion complete);
    // Adds an import batch (already applied to the projection) with its passwords in one write.
    void importProfiles(DocumentId id, QByteArray projection, std::shared_ptr<ImportedPasswords> passwords, QStringList consumers,
                        QString reservation, Completion complete);
    // Writes an mRemoteNG XML/CSV export to a separate file; the open file is not changed.
    void exportFile(DocumentId id, QString path, ExportOptions options, Completion complete);
    void close(DocumentId id, Completion complete);
    void lock(DocumentId id);
    void acknowledgeWarning(DocumentId id);
    std::optional<DocumentStatus> status(DocumentId id) const;
    void setFault(DocumentId id, AtomicFile::Fault fault, Completion complete);
    void setCommitGate(DocumentId id, AtomicFile::CommitGate gate, Completion complete);
signals:
    void stateChanged(DocumentId id);
    void locked(DocumentId id);
    void operationThread(DocumentId id, QThread* performedOn);
private:
    struct Slot;
    struct State;
    using Operation = std::function<void(State&, DocumentReply&, const ConnectionXml::Canceled&)>;
    DocumentId createSlot(QString path);
    void execute(DocumentId id, QString reservation, Operation operation, Completion complete);
    QThread workerThread_;
    QObject* worker_ = nullptr;
    std::shared_ptr<State> state_;
    QHash<DocumentId, std::shared_ptr<Slot>> slots_;
};
