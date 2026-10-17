#pragma once
#include "ConnectionMetadata.h"
#include "PasswordEdit.h"
#include <QHash>
#include <QSet>
#include <QStringList>
#include <map>
#include <memory>

// mRemoteNG "Export to File" choices.
struct ExportOptions {
    enum class Format { Xml, Csv } format = Format::Xml;
    QString scope; // Profile id of the exported folder/connection; empty exports everything.
    bool username = true, password = true, domain = true, inheritance = true;
};
// Passwords of a file being imported, by its own profile ids.
struct ImportedPasswords { std::map<QString, Secret> local; QSet<QString> inherited; };

// XML/crypto representation, owned exclusively by the document worker.
// Checks portable bindings; DocumentStore must also enforce runtime/revision gates.
class ConnectionXml {
public:
    static constexpr qsizetype MaxFile = 32 * 1024 * 1024, MaxInner = 16 * 1024 * 1024;
    using Canceled = std::function<bool()>;
    static std::optional<ConnectionXml> read(const QByteArray& bytes, const Secret* master = nullptr, const Canceled& canceled = {});
    static std::optional<ConnectionXml> create(const Canceled& canceled = {});
    ConnectionXml clone() const;
    ConnectionXml(ConnectionXml&&) noexcept;
    ConnectionXml& operator=(ConnectionXml&&) noexcept;
    ~ConnectionXml();
    QString version() const;
    quint32 iterations() const;
    bool fullEncryption() const;
    std::optional<bool> passwordProtected() const;
    bool needsUnlock() const;
    QStringList profileIds() const;
    bool requiresIdRepair() const;
    std::optional<QByteArray> projection(const Canceled& canceled = {}) const;
    std::optional<QByteArray> patch(const Secret& master, const QByteArray& projection, const Canceled& canceled = {});
    bool unlock(const Secret& master, const Canceled& canceled = {});
    bool authorizeIdRepair();
    bool rename(const QString& id, const QString& name);
    bool setPassword(const Secret& master, const QString& owner, const Secret& value, const Canceled& canceled = {});
    bool setPasswordInheritance(const Secret& master, const QString& owner, bool inherited, const Canceled& canceled = {});
    bool setFullEncryption(const Secret& master, bool enabled, const Canceled& canceled = {});
    bool changeMaster(const Secret& previous, const Secret& replacement, const Canceled& canceled = {});
    bool resetMaster(const Secret& previous, const Canceled& canceled = {});
    QString fileId() const;
    bool credentialReviewRequired() const;
    bool hasCredentials() const;
    QHash<QString, PasswordState> passwordStates(const Canceled& canceled = {}) const;
    bool authorizePasswords(const Secret& master, const QString& editedOwner, const std::vector<PasswordAuthorization>& reviewed, const Canceled& canceled = {});
    bool reviewPasswords(const Secret& master, const std::vector<PasswordAuthorization>& reviewed, const Canceled& canceled = {});
    bool reviewCredentials(const Secret& master, const ConnectionBindings& reviewed, const Canceled& canceled = {});
    std::optional<SessionTarget> committedAccount(const QString& consumer) const;
    // A folder's own or inherited saved password, for filling connections in
    // that folder that have none. No account association is required.
    std::optional<Secret> folderPassword(const Secret& master, const QString& folder, const Canceled& canceled = {}) const;
    std::optional<Secret> password(const Secret& master, const QString& consumer, const SessionTarget& committedTarget,
                                   const Canceled& canceled = {}) const;
    std::optional<QByteArray> serialize(const Secret& master, const Canceled& canceled = {}) const;
    std::optional<ImportedPasswords> importPasswords(const Secret& master, const Canceled& canceled = {}) const;
    // Binds each consumer's saved password to its committed account, as a typed password is.
    bool authorizeConsumers(const Secret& master, const QStringList& consumers, const Canceled& canceled = {});
    // Records whose saved password is currently bound to their committed account.
    std::vector<ConnectionBinding> boundRecords(const Canceled& canceled = {}) const;
    // After an in-app edit: rebinds previously bound consumers whose host or port changed while
    // their account and password did not. Never authorizes anything that was unbound before.
    bool followAccountEdits(const Secret& master, const std::vector<ConnectionBinding>& previouslyBound, const Canceled& canceled = {});
    // A standalone mRemoteNG file: default mR3m key, no CMRemote file extensions.
    std::optional<QByteArray> exportFile(const Secret& master, const ExportOptions& options, const Canceled& canceled = {}) const;
private:
    struct Impl;
    bool rekeyMaster(const Secret& previous, const Secret& replacement, const Canceled& canceled);
    explicit ConnectionXml(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
