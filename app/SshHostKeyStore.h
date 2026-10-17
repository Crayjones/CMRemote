#pragma once
#include "AtomicFile.h"
#include <QHash>
#include <optional>

struct SshHostKey {
    QString host;
    quint16 port = 22;
    QString algorithm;
    QByteArray key;
    QString fingerprint;
    bool operator==(const SshHostKey& other) const {
        return host == other.host && port == other.port && algorithm == other.algorithm && key == other.key && fingerprint == other.fingerprint;
    }
};
SshHostKey makeSshHostKey(const QString& host, quint16 port, const QByteArray& key);

enum class SshHostKeyStatus { Unknown, Trusted, Changed };
enum class SshHostKeyDecision { UseTrusted, TrustAndSave, ReplaceAndConnect, ConnectOnce, Cancel, AcknowledgeWarning };
struct SshHostKeyInspection {
    QString ticket;
    SshHostKeyStatus status = SshHostKeyStatus::Unknown;
    SshHostKey candidate;
    std::optional<SshHostKey> previous;
};
struct SshHostKeyReply {
    FileResult result;
    std::optional<SshHostKeyInspection> inspection;
    bool allowed = false, committed = false, conflict = false, canceled = false;
    quint64 generation = 0;
    Qt::HANDLE performedOn = nullptr;
};

// Blocking persistence primitive; production calls belong to SshHostKeyService's thread.
class SshHostKeyStore {
public:
    explicit SshHostKeyStore(QString path);
    SshHostKeyReply inspect(const SshHostKey& candidate);
    SshHostKeyReply decide(const QString& ticket, SshHostKeyDecision decision);
    void cancel(const QString& ticket);
    void setFault(AtomicFile::Fault fault);
    void setCommitGate(AtomicFile::CommitGate gate);
private:
    struct Pending {
        SshHostKeyInspection inspection;
        QByteArray revision;
        bool exists = false, committed = false;
        QString warning;
    };
    FileResult load();
    AtomicFile file_;
    QVector<SshHostKey> records_;
    QHash<QString, Pending> pending_;
};
