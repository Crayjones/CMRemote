#include "OpenFileList.h"
#include "AtomicFile.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QThread>

namespace {
constexpr qsizetype maxBytes = 256 * 1024;
QStringList decode(const QByteArray& bytes) {
    const auto object = QJsonDocument::fromJson(bytes).object(); QStringList paths;
    if (object.value("version") != 1) return {};
    for (const auto& value : object.value("files").toArray())
        if (const auto path = value.toString(); QFileInfo(path).isAbsolute() && !paths.contains(path)) paths << path;
    return paths;
}
}
OpenFileList::OpenFileList(QString path, QObject* parent) : QObject(parent), path_(std::move(path)) {
    thread_ = new QThread; thread_->setObjectName("openFileListWorker");
    worker_ = new QObject; worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_->start();
}
OpenFileList::~OpenFileList() {
    // Quit behind any queued write so the last list reaches disk.
    auto* thread = thread_;
    QMetaObject::invokeMethod(worker_, [thread] { thread->quit(); }, Qt::QueuedConnection);
    thread_->wait(); delete thread_;
}
void OpenFileList::load(std::function<void(QStringList)> complete) {
    const QPointer<OpenFileList> owner(this);
    QMetaObject::invokeMethod(worker_, [owner, path = path_, complete = std::move(complete)]() mutable {
        AtomicFile file(path); QByteArray bytes; bool exists = false; QStringList paths;
        if (!path.isEmpty() && file.load(bytes, exists, maxBytes) && exists) paths = decode(bytes);
        QMetaObject::invokeMethod(owner, [owner, paths, complete = std::move(complete)] {
            if (!owner) return;
            owner->saved_ = paths; owner->loaded_ = true; complete(paths);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}
void OpenFileList::save(QStringList paths) {
    // Before the stored list is known, a write would discard it.
    if (!loaded_ || paths == saved_ || path_.isEmpty()) return;
    saved_ = paths;
    QJsonArray files; for (const auto& path : paths) files.append(path);
    const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"files", files}}).toJson(QJsonDocument::Compact);
    QMetaObject::invokeMethod(worker_, [path = path_, bytes] {
        AtomicFile file(path); QByteArray current; bool exists = false;
        if (file.load(current, exists, maxBytes) && current != bytes) file.replace(bytes, maxBytes, "open connection files");
    }, Qt::QueuedConnection);
}
