#pragma once
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QNetworkReply>
#include <optional>
class QLibrary;
class QNetworkAccessManager;

// Cisco's prebuilt OpenH264 carries Cisco's H.264 patent license only when each user's device
// downloads it separately. CMRemote never ships it: this downloads, verifies and loads it at runtime.
namespace OpenH264 {
inline constexpr auto Credit = "OpenH264 Video Codec provided by Cisco Systems, Inc.";
inline constexpr auto Version = "2.6.0";
struct Binary {
    QUrl url;          // Cisco's bzip2 download
    QString fileName;  // installed name, beside its verified checksum
    QByteArray sha256; // hex, of the decompressed library
};
// The pinned Cisco binary for this build's platform and CPU, if Cisco publishes one.
std::optional<Binary> platformBinary();
// Hands the library path to FreeRDP; empty turns H.264 off for sessions started afterwards.
void useLibrary(const QString& path);
}

class OpenH264Codec final : public QObject {
    Q_OBJECT
public:
    enum class State { Off, Unsupported, Downloading, Ready, Failed };
    explicit OpenH264Codec(QString directory, QObject* parent = nullptr,
                           std::optional<OpenH264::Binary> binary = OpenH264::platformBinary());
    ~OpenH264Codec() override;
    // Enabled: use the verified download, fetching it first when missing. Disabled: stop using it.
    void setEnabled(bool enabled);
    bool enabled() const { return enabled_; }
    State state() const { return state_; }
    QString error() const { return error_; }
    QString libraryPath() const;
    QString statusText() const;
signals:
    void stateChanged();
private:
    void download();
    bool activate();
    void setState(State state, QString error = {});
    QString directory_;
    std::optional<OpenH264::Binary> binary_;
    QNetworkAccessManager* network_ = nullptr;
    QPointer<QNetworkReply> reply_;
    bool enabled_ = false;
    State state_ = State::Off;
    QString error_;
    QLibrary* module_ = nullptr;
};
