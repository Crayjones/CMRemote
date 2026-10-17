#pragma once
#include "UpdateCheck.h"
#include <QCryptographicHash>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <memory>
#include <optional>
class QNetworkAccessManager;
class QNetworkReply;

namespace Updates {
// The installer this build can apply by itself for `version`, or nothing when
// this kind of install (portable zip, tarball, Flatpak, development build) cannot
// update itself.
std::optional<Asset> installerFor(const QList<Asset>& assets, const QString& version);
// "SHA256SUMS.txt" lines: "<hex>  <name>".
QString checksumFor(const QByteArray& sums, const QString& name);
}

// Automatic updates: checks GitHub at startup and daily, downloads the matching
// installer into <data>/updates, verifies its SHA-256 against the published
// digest, and installs it after the application exits.
class AutoUpdater final : public QObject {
    Q_OBJECT
public:
    explicit AutoUpdater(QString dataDirectory, QObject* parent = nullptr, QUrl source = QUrl(Updates::LatestReleaseApi));
    ~AutoUpdater() override;
    void setEnabled(bool enabled);
    bool enabled() const { return enabled_; }
    void checkNow();
    bool busy() const { return check_ || download_; }
    // A verified installer is waiting.
    bool ready() const { return !readyPath_.isEmpty(); }
    QString readyVersion() const { return readyVersion_; }
    // Starts a detached helper that waits for this process to exit, installs the
    // update and optionally starts the new version. Call while quitting.
    bool install(bool relaunch, QString* error = nullptr);
signals:
    // Plain-text progress for the Notifications panel.
    void log(const QString& text);
    void readyToInstall(const QString& version);
private:
    void finishCheck(const Updates::Release& release);
    void download(const Updates::Asset& asset, const QString& version, const QString& sha256);
    void fail(const QString& text);
    QString directory() const;
    QString dataDirectory_;
    QUrl source_;
    bool enabled_ = false;
    QTimer daily_;
    QNetworkAccessManager* network_;
    QPointer<UpdateChecker> check_;
    QPointer<QNetworkReply> download_;
    std::unique_ptr<QFile> partial_;
    std::unique_ptr<QCryptographicHash> hash_;
    QString readyPath_, readyVersion_, downloading_;
};
