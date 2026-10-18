#pragma once
#include "UpdateCheck.h"
#include <QCryptographicHash>
#include <QFile>
#include <QLockFile>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <memory>
#include <optional>
class QNetworkAccessManager;
class QNetworkReply;

namespace Updates {
// How this copy of CMRemote was installed, and whether it can replace itself.
struct Installation {
    enum Kind { None, MacBundle, AppImage };
    Kind kind = None;
    QString target;       // the .app bundle or AppImage file the helper replaces
    QString identifier;   // macOS bundle identifier the new bundle must carry
    QString architecture; // arm64, x64 or x86_64
    QString reason;       // why this copy cannot update itself (kind == None)
    bool supported() const { return kind != None; }
};
// Only a GitHub-release package (build-info.json with update_channel "github" and
// this version) at a writable location qualifies; development builds never do.
Installation detectInstallation();
// The published package that replaces `installation` for `version`.
std::optional<Asset> installerFor(const QList<Asset>& assets, const QString& version, const Installation& installation);
// "SHA256SUMS.txt" lines: "<hex>  <name>". Missing or conflicting entries → empty.
QString checksumFor(const QByteArray& sums, const QString& name);
}

// Automatic updates. Checks GitHub at startup and daily while automatic updates
// are on (or once when asked), downloads the matching package into
// <data>/updates, verifies its size and SHA-256 and records it in pending.json.
// Nothing installs without the user's choice: accept() for the ready version (or
// skip() it). Installation happens only after CMRemote exits: prepare() starts an inert
// helper that waits for this process, and commit() during the final close lets
// it replace the application. Without a commit the helper changes nothing.
class AutoUpdater final : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString dataDirectory;
        QUrl source = QUrl(Updates::LatestReleaseApi);
        std::optional<Updates::Installation> installation; // detected when empty
        QNetworkAccessManager* network = nullptr;          // tests serve releases from local files
        qint64 parentPid = 0;                              // process the helper waits for (0: this one)
        int firstCheckMs = 20 * 1000;
        int helperTimeoutMs = 10 * 1000;
    };
    explicit AutoUpdater(Options options, QObject* parent = nullptr);
    ~AutoUpdater() override;
    // Called once the stored preference is known; nothing is scheduled before.
    void setAutomatic(bool automatic);
    bool automatic() const { return automatic_; }
    // manual: the user asked. Reports through checked() and downloads even when
    // automatic updates are off; it also offers a skipped or failed version again.
    void checkNow(bool manual = false);
    bool busy() const { return check_ || reply_ || hashing_; }
    bool ready() const { return !readyPath_.isEmpty(); }
    QString readyVersion() const { return readyVersion_; }
    // A verified update the user may be asked to install.
    bool offered() const;
    // The user chose to install the offered update; it installs when CMRemote quits.
    void accept();
    void decline();
    bool installOnExit() const;
    // Never offer this version again unless the user checks for updates.
    void skip();
    const Updates::Installation& installation() const { return installation_; }
    QString directory() const;

    // Exit handoff: prepare() rehashes the package off the GUI thread and starts
    // the helper; preparationFinished reports whether it acknowledged.
    void prepare();
    bool preparing() const { return preparing_; }
    bool prepared() const { return !session_.isEmpty(); }
    // Final close only: allows the prepared helper to install once this process exits.
    bool commit(bool relaunch, QString* error = nullptr);
signals:
    // Plain-text progress for the Notifications panel.
    void log(const QString& text);
    void readyToInstall(const QString& version);
    // Result of a manual check. installable: this copy downloads and installs it.
    void checked(bool ok, const Updates::Release& latest, const QString& error, bool installable);
    void preparationFinished(bool ok, const QString& error);
private:
    void recover();
    void finishCheck(const Updates::Release& release);
    void reportCheck(bool ok, const Updates::Release& latest, const QString& error, bool installable);
    void startCheck();
    void download(const Updates::Asset& asset, const QString& version, const QString& sha256);
    void verifyCached(const QString& path, const QString& version, const QString& sha256, qint64 size, bool fresh);
    void markReady(const QString& path, const QString& version, const QString& sha256, qint64 size);
    void cancel();
    void discardReady();
    void fail(const QString& text);
    void failPreparation(const QString& text);
    QString helperScript() const;
    void startHelper();
    Options options_;
    Updates::Installation installation_;
    bool automatic_ = false, scheduled_ = false, manual_ = false, reportCheck_ = false, hashing_ = false, preparing_ = false;
    bool usable_ = false, checkAfterVerify_ = false;
    quint64 generation_ = 0;
    QTimer daily_;
    QNetworkAccessManager* network_;
    std::unique_ptr<QLockFile> lock_;
    QPointer<UpdateChecker> check_;
    QPointer<QNetworkReply> reply_;
    std::unique_ptr<QFile> partial_;
    std::unique_ptr<QCryptographicHash> hash_;
    qint64 expectedSize_ = 0;
    std::optional<Updates::Release> latest_;
    QString readyPath_, readyVersion_, readySha_, manualVersion_, failedVersion_, skippedVersion_, acceptedVersion_, unusable_;
    qint64 readySize_ = 0;
    QString session_;
};
