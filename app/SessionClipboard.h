#pragma once
#include "ClipboardTransfer.h"
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <memory>

class ClipboardEndpoint : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void watch() = 0;
    virtual std::optional<QString> readText() = 0;
    virtual void writeText(const QString& text) = 0;
    // Local files on the clipboard (file manager copies); empty for anything else.
    virtual QStringList readFiles() { return {}; }
    // Puts local files on the clipboard for file managers (Finder, Explorer, Nautilus, Dolphin).
    virtual void writeFiles(const QStringList& paths) { Q_UNUSED(paths); }
    std::optional<QString> observe();
    void claim(QObject* owner);
    void release(QObject* owner);
    quint64 route() const { return route_; }
    quint64 revision() const { return revision_; }
    bool owns(QObject* owner) const { return owner_ == owner; }
    bool echo(QObject* owner, const QString& text) const;
    bool echoFiles(QObject* owner, const QStringList& paths) const;
    void writeRemote(QObject* owner, const QString& text);
    void writeRemoteFiles(QObject* owner, const QStringList& paths);
signals:
    void changed();
    void routeChanged();
private:
    QPointer<QObject> owner_, remoteSource_;
    quint64 route_ = 0, revision_ = 0;
    bool observed_ = false;
    std::optional<QString> last_, remoteText_;
    QStringList remoteFiles_;
};
std::shared_ptr<ClipboardEndpoint> systemClipboardEndpoint();

class SessionClipboard final : public QObject {
    Q_OBJECT
public:
    explicit SessionClipboard(std::shared_ptr<ClipboardEndpoint> endpoint, QObject* parent = nullptr);
    ~SessionClipboard() override;
    bool enabled() const { return enabled_; }
    void setEnabled(bool enabled);
    void setFocused(bool focused);
    void receive(ClipboardDelivery delivery);
    // Files the remote computer copied, already saved locally; same checks as text.
    void receiveFiles(const QStringList& paths, ClipboardStamp stamp);
    void reset();
signals:
    void configure(ClipboardControl control);
    void localText(QString text, ClipboardStamp stamp);
    // Sent with every synchronization, before the text; an empty list withdraws earlier files.
    void localFiles(QStringList paths, ClipboardStamp stamp);
private:
    ClipboardStamp stamp() const;
    void synchronize();
    std::shared_ptr<ClipboardEndpoint> endpoint_;
    quint64 permission_ = 0;
    bool enabled_ = false, focused_ = false, writing_ = false;
};
Q_DECLARE_METATYPE(ClipboardControl)
