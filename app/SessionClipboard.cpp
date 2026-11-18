#include "SessionClipboard.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QThread>
#include <QUrl>

namespace {
class SystemClipboard final : public ClipboardEndpoint {
public:
    void watch() override {
        if (clipboard_) return;
        clipboard_ = QGuiApplication::clipboard();
        connect(clipboard_, &QClipboard::dataChanged, this, &ClipboardEndpoint::changed);
    }
    std::optional<QString> readText() override {
        Q_ASSERT(QThread::currentThread() == thread());
        const auto* mime = clipboard_->mimeData(QClipboard::Clipboard);
        if (!mime || !mime->hasText()) return {};
        auto text = mime->text();
        return text.size() <= 512 * 1024 - 1 ? std::optional<QString>(std::move(text)) : std::nullopt;
    }
    void writeText(const QString& text) override {
        Q_ASSERT(QThread::currentThread() == thread()); clipboard_->setText(text, QClipboard::Clipboard);
    }
    QStringList readFiles() override {
        Q_ASSERT(QThread::currentThread() == thread());
        const auto* mime = clipboard_->mimeData(QClipboard::Clipboard);
        if (!mime || !mime->hasUrls()) return {};
        QStringList paths;
        for (const auto& url : mime->urls()) {
            // Mixed or remote URLs are not a file copy.
            if (!url.isLocalFile() || url.toLocalFile().isEmpty()) return {};
            paths << url.toLocalFile();
        }
        return paths;
    }
    void writeFiles(const QStringList& paths) override {
        Q_ASSERT(QThread::currentThread() == thread());
        auto* mime = new QMimeData; QList<QUrl> urls; QByteArray gnome("copy");
        for (const auto& path : paths) { urls << QUrl::fromLocalFile(path); gnome += '\n' + urls.last().toEncoded(); }
        mime->setUrls(urls);
        // GNOME/MATE file managers paste only their own format.
        mime->setData("x-special/gnome-copied-files", gnome); mime->setData("x-special/mate-copied-files", gnome);
        clipboard_->setMimeData(mime, QClipboard::Clipboard);
    }
private:
    QClipboard* clipboard_ = nullptr;
};
}
std::shared_ptr<ClipboardEndpoint> systemClipboardEndpoint() { return std::make_shared<SystemClipboard>(); }
std::optional<QString> ClipboardEndpoint::observe() {
    auto text = readText();
    if (!observed_ || text != last_) {
        observed_ = true; last_ = text; ++revision_;
        if (text != remoteText_) { remoteSource_.clear(); remoteText_.reset(); remoteFiles_.clear(); }
    }
    return text;
}
void ClipboardEndpoint::claim(QObject* owner) {
    if (owner_ == owner) return;
    owner_ = owner; ++route_; emit routeChanged();
}
void ClipboardEndpoint::release(QObject* owner) {
    if (owner_ == owner) {
        last_.reset(); remoteText_.reset(); remoteFiles_.clear(); remoteSource_.clear(); observed_ = false; ++revision_;
        claim(nullptr);
    } else if (remoteSource_ == owner) { remoteText_.reset(); remoteFiles_.clear(); remoteSource_.clear(); }
}
bool ClipboardEndpoint::echo(QObject* owner, const QString& text) const { return remoteSource_ == owner && remoteText_ && *remoteText_ == text; }
void ClipboardEndpoint::writeRemote(QObject* owner, const QString& text) {
    remoteSource_ = owner; remoteText_ = text; remoteFiles_.clear(); last_ = text; observed_ = true; ++revision_;
    writeText(text);
}
bool ClipboardEndpoint::echoFiles(QObject* owner, const QStringList& paths) const {
    return remoteSource_ == owner && !remoteFiles_.isEmpty() && remoteFiles_ == paths;
}
void ClipboardEndpoint::writeRemoteFiles(QObject* owner, const QStringList& paths) {
    writeFiles(paths);
    // Whatever text the platform derives from the file URLs is part of the same echo.
    remoteSource_ = owner; remoteFiles_ = paths; last_ = readText(); remoteText_ = last_; observed_ = true; ++revision_;
}
SessionClipboard::SessionClipboard(std::shared_ptr<ClipboardEndpoint> endpoint, QObject* parent)
    : QObject(parent), endpoint_(std::move(endpoint)) {
    connect(endpoint_.get(), &ClipboardEndpoint::changed, this, [this] {
        if (enabled_ && focused_ && !writing_ && endpoint_->owns(this)) synchronize();
    });
    connect(endpoint_.get(), &ClipboardEndpoint::routeChanged, this, [this] {
        if (enabled_) emit configure({true, focused_ && endpoint_->owns(this), stamp()});
    });
}
SessionClipboard::~SessionClipboard() { reset(); }
ClipboardStamp SessionClipboard::stamp() const { return {permission_, endpoint_->route(), endpoint_->revision()}; }
void SessionClipboard::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled; ++permission_;
    if (enabled_) {
        endpoint_->watch();
        if (focused_) { endpoint_->claim(this); synchronize(); }
        else emit configure({true, false, stamp()});
    } else {
        endpoint_->release(this); emit configure({false, false, stamp()});
    }
}
void SessionClipboard::setFocused(bool focused) {
    if (focused_ == focused) return;
    focused_ = focused;
    if (!enabled_) return;
    if (focused_) { endpoint_->claim(this); synchronize(); }
    else emit configure({true, false, stamp()});
}
void SessionClipboard::synchronize() {
    if (!enabled_ || !focused_ || !endpoint_->owns(this)) return;
    auto text = endpoint_->observe();
    emit configure({true, true, stamp()});
    // Files this session saved from the remote clipboard are its own echo: never offered back.
    auto files = endpoint_->readFiles();
    if (endpoint_->echoFiles(this, files)) return;
    emit localFiles(std::move(files), stamp());
    if (text && !endpoint_->echo(this, *text)) emit localText(std::move(*text), stamp());
}
void SessionClipboard::receive(ClipboardDelivery delivery) {
    if (!enabled_ || !endpoint_->owns(this) || delivery.stamp.permission != permission_ || delivery.stamp.route != endpoint_->route()) return;
    endpoint_->observe();
    if (delivery.stamp.host != endpoint_->revision() || !encodeClipboardText(delivery.text)) return;
    writing_ = true; endpoint_->writeRemote(this, delivery.text); writing_ = false;
    emit configure({true, focused_, stamp()});
}
void SessionClipboard::receiveFiles(const QStringList& paths, ClipboardStamp stamp) {
    if (paths.isEmpty() || !enabled_ || !endpoint_->owns(this) || stamp.permission != permission_ || stamp.route != endpoint_->route()) return;
    endpoint_->observe();
    if (stamp.host != endpoint_->revision()) return;
    writing_ = true; endpoint_->writeRemoteFiles(this, paths); writing_ = false;
    emit configure({true, focused_, this->stamp()});
}
void SessionClipboard::reset() { setEnabled(false); focused_ = false; }
