#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <optional>
#include <QNetworkReply>
class QNetworkAccessManager;

namespace Updates {
inline constexpr auto Repository = "https://github.com/Crayjones/CMRemote";
inline constexpr auto LatestReleaseApi = "https://api.github.com/repos/Crayjones/CMRemote/releases/latest";
QString currentVersion();
// Dotted numeric comparison; a leading "v" and any "-suffix" are ignored. Unparseable → nullopt.
std::optional<int> compareVersions(const QString& left, const QString& right);
struct Asset { QString name, url, sha256; qint64 size = 0; }; // sha256: lowercase hex from GitHub's asset digest, if published; size 0: unknown
struct Release { QString version, url; QList<Asset> assets; };
// GitHub's "latest release" JSON: tag_name, html_url and assets (release downloads only).
std::optional<Release> parseLatestRelease(const QByteArray& json);
}

Q_DECLARE_METATYPE(Updates::Release)

// Asks GitHub for the latest published release. Nothing is downloaded or installed.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    // network: shared manager (tests redirect requests to local files); owned when null.
    explicit UpdateChecker(QObject* parent = nullptr, QUrl source = QUrl(Updates::LatestReleaseApi), QNetworkAccessManager* network = nullptr);
    ~UpdateChecker() override;
    void check();
    bool running() const { return reply_; }
signals:
    void finished(bool ok, const Updates::Release& latest, const QString& error);
private:
    QUrl source_;
    QNetworkAccessManager* network_;
    QPointer<QNetworkReply> reply_;
};
