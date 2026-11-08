#pragma once
#include "UpdateCheck.h"
#include <QCryptographicHash>
#include <QFile>
#include <QLockFile>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>
class QNetworkAccessManager;
class QNetworkReply;

namespace Updates {
// How this copy of CMRemote was installed, and whether it can replace itself.
struct Installation {
    enum Kind { None, MacBundle, AppImage, WindowsSetup };
    Kind kind = None;
    QString target;       // the .app bundle, AppImage file or setup install folder the helper replaces
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
// Windows, setup programs older than staged updates: starts the setup a committed update
// chose, after CMRemote's window has closed (main() calls it last). Setup waits for CMRemote
// to exit, installs, and reopens it.
void runPendingSetup();
}

// Automatic updates. Checks GitHub soon after startup and daily while automatic updates
// are on (or once when asked), downloads the matching package into
// <data>/updates, verifies its size and SHA-256 and records it in pending.json.
// Nothing installs without the user's choice: accept() for the ready version (or
// skip() it). Installation happens only after CMRemote exits: prepare() starts a helper
// that copies the new version beside this one while CMRemote shows its progress, then
// waits for this process; commit() during the final close lets it swap the two. Without
// a commit the helper removes its copy and changes nothing. On Windows the new version's
// setup program is that helper (started with administrator permission); a setup without
// staged updates instead runs after CMRemote has closed (runPendingSetup()).
class AutoUpdater final : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString dataDirectory;
        QUrl source = QUrl(Updates::LatestReleaseApi);
        std::optional<Updates::Installation> installation; // detected when empty
        QNetworkAccessManager* network = nullptr;          // tests serve releases from local files
        qint64 parentPid = 0;                              // process the helper waits for (0: this one)
        int firstCheckMs = 3 * 1000;     // soon after startup
        int helperTimeoutMs = 10 * 1000;  // until the helper starts
        int stagingTimeoutMs = 5 * 60 * 1000; // until the new version is copied
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
    // CMRemote shows the install progress itself (false: an older Windows setup shows its own).
    bool showsInstallProgress() const;
    QString directory() const;

    // Exit handoff: prepare() rehashes the package off the GUI thread and starts the
    // helper; preparationProgress follows its copy, preparationFinished reports whether
    // the new version is ready to swap in.
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
    // The download a manual check reported as installable stopped without an update.
    void downloadFailed(const QString& version, const QString& error);
    // Percent of the new version copied beside this one; -1 while the size is unknown.
    void preparationProgress(int percent);
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
    void failDownload(const QString& text);
    void failPreparation(const QString& text);
    QString helperScript() const;
    void startHelper();
    void watchHelper(const QString& session, quint64 generation, std::function<bool()> alive, std::function<void()> stop);
    Options options_;
    Updates::Installation installation_;
    bool automatic_ = false, scheduled_ = false, manual_ = false, reportCheck_ = false, hashing_ = false, preparing_ = false;
    bool usable_ = false, checkAfterVerify_ = false, reportDownload_ = false, legacySetup_ = false;
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
    QString downloadVersion_;
    QString readyPath_, readyVersion_, readySha_, manualVersion_, failedVersion_, skippedVersion_, acceptedVersion_, unusable_;
    qint64 readySize_ = 0;
    QString session_;
};
