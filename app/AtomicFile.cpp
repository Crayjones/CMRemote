#include "AtomicFile.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

static QByteArray digest(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
AtomicFile::AtomicFile(QString path) : path_(QFileInfo(path).absoluteFilePath()) {}
void AtomicFile::reset() { revision_.clear(); inspected_ = false; exists_ = false; }
bool AtomicFile::safePath() const {
    const QFileInfo file(path_), parent(file.absolutePath());
    if (!parent.isDir() || parent.canonicalFilePath() != parent.absoluteFilePath()) return false;
    if (file.isSymLink() || (file.exists() && !file.isFile())) return false;
    const QFileInfo lock(path_ + ".lock"); return !lock.isSymLink() && (!lock.exists() || lock.isFile());
}
bool AtomicFile::read(QByteArray& bytes, bool& exists, qsizetype limit) const {
    if (limit < 0 || !safePath()) return false;
    exists = QFileInfo::exists(path_); if (!exists) { bytes.clear(); return true; }
    QFile file(path_); if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || file.size() > limit) return false;
    bytes = file.read(limit + 1); return bytes.size() == file.size() && bytes.size() <= limit && file.error() == QFileDevice::NoError;
}
bool AtomicFile::load(QByteArray& bytes, bool& exists, qsizetype limit) {
    reset(); if (!read(bytes, exists, limit)) return false;
    revision_ = digest(bytes); exists_ = exists; inspected_ = true; return true;
}
bool AtomicFile::unchanged(qsizetype limit) const {
    QByteArray bytes; bool exists;
    return inspected_ && read(bytes, exists, limit) && exists == exists_ && (!exists || digest(bytes) == revision_);
}
FileResult AtomicFile::replace(const QByteArray& bytes, qsizetype limit, const QString& label, const std::function<bool()>& prerequisite) {
    const auto failure = [&](const QString& message) { return FileResult{false, message, {}}; };
    if (!inspected_ || bytes.size() > limit || !safePath()) return failure("Cannot save " + label + ": unsafe path or invalid state.");
    QLockFile lock(path_ + ".lock"); lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) return failure("Another application is writing this " + label + ". Try again after it finishes.");
    if (!unchanged(limit)) return failure(label + " changed outside this window. Reopen it before saving.");
    QSaveFile file(path_); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) || file.write(bytes) != bytes.size())
        return failure("Cannot write " + label + ".");
    if (fault_ == Fault::BeforeReplace) { file.cancelWriting(); return failure(label + " replacement failed."); }
    const auto commit = [&] { return unchanged(limit) && (!prerequisite || prerequisite()) && file.commit(); };
    if (!(gate_ ? gate_(commit) : commit())) { file.cancelWriting(); return failure(label + " save canceled, conflicted or failed before replacement."); }
    revision_ = digest(bytes); exists_ = true;
    bool synced = fault_ != Fault::DirectorySync;
#ifdef Q_OS_UNIX
    const auto parent = QFile::encodeName(QFileInfo(path_).absolutePath());
    const int fd = ::open(parent.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) synced = false;
    else { if (::fsync(fd) != 0) synced = false; ::close(fd); }
#endif
    return {true, {}, synced ? QString() : label + " saved, but directory sync failed. Durability after a system crash is not guaranteed."};
}
