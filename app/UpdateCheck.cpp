#include "UpdateCheck.h"
#include "Version.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>

namespace Updates {
QString currentVersion() { return QStringLiteral(RM_VERSION); }
static std::optional<QList<int>> parts(QString version) {
    version = version.trimmed();
    if (version.startsWith('v', Qt::CaseInsensitive)) version.remove(0, 1);
    version = version.section('-', 0, 0).section('+', 0, 0);
    static const QRegularExpression shape("^\\d+(\\.\\d+){0,3}$");
    if (!shape.match(version).hasMatch()) return std::nullopt;
    QList<int> numbers;
    for (const auto& part : version.split('.')) numbers.append(part.toInt());
    while (numbers.size() < 4) numbers.append(0);
    return numbers;
}
std::optional<int> compareVersions(const QString& left, const QString& right) {
    const auto a = parts(left), b = parts(right);
    if (!a || !b) return std::nullopt;
    for (int i = 0; i < 4; ++i) if ((*a)[i] != (*b)[i]) return (*a)[i] < (*b)[i] ? -1 : 1;
    return 0;
}
std::optional<Release> parseLatestRelease(const QByteArray& json) {
    const auto object = QJsonDocument::fromJson(json).object();
    const auto tag = object.value("tag_name").toString(), url = object.value("html_url").toString();
    if (!parts(tag) || !url.startsWith(QString(Repository) + "/")) return std::nullopt;
    auto version = tag.trimmed(); if (version.startsWith('v', Qt::CaseInsensitive)) version.remove(0, 1);
    Release release{version, url, {}};
    static const QRegularExpression digest("^sha256:([0-9a-fA-F]{64})$");
    const QString downloads = QString(Repository) + "/releases/download/";
    for (const auto& value : object.value("assets").toArray()) {
        const auto asset = value.toObject();
        const auto name = asset.value("name").toString(), link = asset.value("browser_download_url").toString();
        // Only this repository's release downloads; a name never names a directory.
        if (name.isEmpty() || name.contains('/') || name.contains('\\') || name.startsWith('.') || !link.startsWith(downloads)) continue;
        const auto match = digest.match(asset.value("digest").toString());
        release.assets.append({name, link, match.hasMatch() ? match.captured(1).toLower() : QString()});
    }
    return release;
}
}

UpdateChecker::UpdateChecker(QObject* parent, QUrl source) : QObject(parent), source_(std::move(source)), network_(new QNetworkAccessManager(this)) {}
void UpdateChecker::check() {
    if (reply_) return;
    QNetworkRequest request(source_);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setHeader(QNetworkRequest::UserAgentHeader, "CMRemote/" + Updates::currentVersion());
    request.setTransferTimeout(15000);
    reply_ = network_->get(request);
    connect(reply_, &QNetworkReply::finished, this, [this, reply = reply_] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            // GitHub answers 404 until the first release is published.
            const bool none = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 404;
            emit finished(false, {}, none ? "No release has been published yet." : reply->errorString());
            return;
        }
        const auto latest = Updates::parseLatestRelease(reply->readAll());
        if (!latest) { emit finished(false, {}, "GitHub returned an unexpected release description."); return; }
        emit finished(true, *latest, {});
    });
}
