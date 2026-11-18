#include "ClipboardFiles.h"
#include <QDateTime>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>
#include <atomic>
#include <limits>

namespace {
// FILEDESCRIPTORW: flags, CLSID, SIZEL, POINTL, attributes, three FILETIMEs, size, WCHAR[260].
constexpr qsizetype DescriptorSize = 592, NameOffset = 72, NameChars = 260;
constexpr quint32 FdAttributes = 0x4, FdWriteTime = 0x20, FdFileSize = 0x40, FdProgressUi = 0x4000;
constexpr quint32 AttributeDirectory = 0x10, AttributeNormal = 0x80;
constexpr quint32 MaxRangeBytes = 8 * 1024 * 1024, ChunkBytes = 512 * 1024;
// 100 ns intervals between 1601-01-01 (FILETIME) and 1970-01-01.
constexpr quint64 FileTimeEpoch = 116444736000000000ull;

quint64 fileTime(const QDateTime& time) {
    const auto ms = time.isValid() ? time.toMSecsSinceEpoch() : 0;
    return ms > 0 ? FileTimeEpoch + quint64(ms) * 10000 : FileTimeEpoch;
}
bool validUtf16(const QString& text) {
    for (qsizetype i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        if (c.isLowSurrogate()) return false;
        if (c.isHighSurrogate() && (++i == text.size() || !text[i].isLowSurrogate())) return false;
    }
    return true;
}
// Server names become host paths: refuse anything a host file system could reinterpret.
bool safeComponent(const QString& name) {
    if (name.isEmpty() || name == "." || name == ".." || name.endsWith('.') || name.endsWith(' ')) return false;
    for (const QChar c : name) if (c.unicode() < 0x20 || c.unicode() == 0x7f || QStringLiteral("<>:\"/\\|?*").contains(c)) return false;
    return true;
}
struct Walk {
    LocalFileList list;
    QStringList names;
    std::vector<quint64> times;
    QString error;
    bool add(const QFileInfo& info, const QString& name) {
        if (int(list.files.size()) >= MaxClipboardFiles) { error = QString("more than %1 files and folders").arg(MaxClipboardFiles); return false; }
        if (name.size() > NameChars - 1) { error = "a path longer than 259 characters (" + name + ")"; return false; }
        const bool directory = info.isDir();
        list.files.push_back({info.absoluteFilePath(), directory ? 0 : quint64(qMax<qint64>(0, info.size())), directory});
        names.push_back(name); times.push_back(fileTime(info.lastModified()));
        if (!directory) return true;
        QDir dir(info.absoluteFilePath());
        if (!dir.isReadable()) { error = "an unreadable folder (" + info.absoluteFilePath() + ")"; return false; }
        const auto entries = dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
        for (const auto& entry : entries) if (!add(entry, name + '\\' + entry.fileName())) return false;
        return true;
    }
};
}

std::shared_ptr<const LocalFileList> buildLocalFileList(const QStringList& paths, QString* error) {
    Walk walk; QSet<QString> roots;
    const auto failed = [&](const QString& reason) -> std::shared_ptr<const LocalFileList> {
        if (error) *error = "Files were not shared: the clipboard holds " + reason + ".";
        return nullptr;
    };
    if (error) error->clear();
    if (paths.isEmpty()) return nullptr;
    for (const auto& path : paths) {
        // Finder and file managers name folders with a trailing slash, which QFileInfo reads as no name.
        const QFileInfo info(QDir::cleanPath(path));
        // Top-level links are followed: the user chose them. Nested ones are skipped by the walk.
        if (!info.exists() || (!info.isFile() && !info.isDir())) return failed("an item that is not a file or folder (" + path + ")");
        const auto name = info.fileName();
        if (name.isEmpty() || !validUtf16(name)) return failed("an item without a usable name (" + path + ")");
        if (roots.contains(name.toLower())) return failed("two items named " + name);
        roots.insert(name.toLower());
        if (!walk.add(info, name)) return failed(walk.error);
    }
    auto& list = walk.list;
    list.descriptor = QByteArray(4 + qsizetype(list.files.size()) * DescriptorSize, '\0');
    auto* data = reinterpret_cast<uchar*>(list.descriptor.data());
    qToLittleEndian<quint32>(quint32(list.files.size()), data);
    for (size_t i = 0; i < list.files.size(); ++i) {
        auto* d = data + 4 + qsizetype(i) * DescriptorSize;
        const auto& file = list.files[i];
        qToLittleEndian<quint32>(FdAttributes | FdWriteTime | FdFileSize | FdProgressUi, d);
        qToLittleEndian<quint32>(file.directory ? AttributeDirectory : AttributeNormal, d + 36);
        qToLittleEndian<quint64>(walk.times[i], d + 56);
        qToLittleEndian<quint32>(quint32(file.size >> 32), d + 64);
        qToLittleEndian<quint32>(quint32(file.size), d + 68);
        const auto& name = walk.names[i];
        for (qsizetype c = 0; c < name.size(); ++c) qToLittleEndian<quint16>(name[c].unicode(), d + NameOffset + c * 2);
    }
    return std::make_shared<const LocalFileList>(std::move(list));
}

std::optional<QByteArray> readLocalFileContents(const LocalFileList& list, quint32 index, quint32 flags, quint64 position, quint32 requested) {
    if (index >= list.files.size() || list.files[index].directory) return {};
    const auto& path = list.files[index].path;
    if (flags & ClipboardFileContentsSize) {
        const QFileInfo info(path);
        if (!info.isFile()) return {};
        QByteArray size(8, '\0'); qToLittleEndian<quint64>(quint64(qMax<qint64>(0, info.size())), size.data()); return size;
    }
    if (!(flags & ClipboardFileContentsRange) || requested > MaxRangeBytes || position > quint64(std::numeric_limits<qint64>::max())) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(qint64(position))) return {};
    auto bytes = file.read(requested);
    if (file.error() != QFileDevice::NoError) return {};
    return bytes;
}

std::optional<RemoteFileList> parseRemoteFileList(const uchar* bytes, qsizetype size) {
    if (!bytes || size < 4) return {};
    const quint32 count = qFromLittleEndian<quint32>(bytes);
    if (count > quint32(MaxClipboardFiles) || size < 4 + qsizetype(count) * DescriptorSize) return {};
    RemoteFileList list; QSet<QString> seen, files, parents;
    for (quint32 i = 0; i < count; ++i) {
        const auto* d = bytes + 4 + qsizetype(i) * DescriptorSize;
        const quint32 flags = qFromLittleEndian<quint32>(d), attributes = qFromLittleEndian<quint32>(d + 36);
        QString name; bool terminated = false;
        for (qsizetype c = 0; c < NameChars; ++c) {
            const quint16 unit = qFromLittleEndian<quint16>(d + NameOffset + c * 2);
            if (!unit) { terminated = true; break; }
            name += QChar(unit);
        }
        if (!terminated || !validUtf16(name)) return {};
        const auto parts = name.split(QRegularExpression("[\\\\/]"));
        for (const auto& part : parts) if (!safeComponent(part)) return {};
        RemoteClipboardFile file{parts.join('/'), (flags & FdAttributes) && (attributes & AttributeDirectory), std::nullopt};
        const auto key = file.path.toLower();
        if (seen.contains(key)) return {};
        // A file cannot also be a parent folder of another entry, in either order.
        for (qsizetype slash = key.indexOf('/'); slash >= 0; slash = key.indexOf('/', slash + 1)) {
            if (files.contains(key.left(slash))) return {};
            parents.insert(key.left(slash));
        }
        seen.insert(key);
        if (!file.directory) {
            if (parents.contains(key)) return {};
            files.insert(key);
            if (flags & FdFileSize) {
                file.size = quint64(qFromLittleEndian<quint32>(d + 64)) << 32 | qFromLittleEndian<quint32>(d + 68);
                if (*file.size > std::numeric_limits<quint64>::max() - list.bytes) return {};
                list.bytes += *file.size;
            }
        }
        list.files.push_back(std::move(file));
    }
    return list;
}

RemoteFileDownload::RemoteFileDownload(RemoteFileList list, QString directory, qint64 timeoutMs)
    : list_(std::move(list)), directory_(std::move(directory)), timeoutMs_(timeoutMs) {}
RemoteFileDownload::~RemoteFileDownload() { file_.reset(); }
bool RemoteFileDownload::fail(const QString& reason) {
    if (error_.isEmpty()) error_ = reason;
    file_.reset(); outstanding_.reset(); return false;
}
void RemoteFileDownload::cancel(const QString& reason) { if (!finished_) fail(reason); }
bool RemoteFileDownload::start() {
    if (started_) return !failed();
    started_ = true;
    if (!directory_.exists()) return fail("The destination folder no longer exists.");
    // Existing names are never replaced: a colliding top-level item gets a " (2)" style name.
    for (const auto& file : list_.files) {
        const auto root = file.path.section('/', 0, 0);
        if (roots_.contains(root)) continue;
        const bool folder = file.directory || file.path.contains('/');
        const QFileInfo info(root);
        const auto base = folder || info.baseName().isEmpty() ? root : info.baseName();
        const auto suffix = folder || info.baseName().isEmpty() ? QString() : "." + info.completeSuffix();
        QString name = root;
        for (int n = 2; directory_.exists(name) || renamed_.contains(name, Qt::CaseInsensitive); ++n) {
            if (n > 9999) return fail("Could not choose a free name for " + root + ".");
            name = QString("%1 (%2)%3").arg(base).arg(n).arg(suffix);
        }
        roots_ << root; renamed_ << name;
    }
    return true;
}
QString RemoteFileDownload::target(const RemoteClipboardFile& file) const {
    const auto root = file.path.section('/', 0, 0);
    return directory_.filePath(renamed_[roots_.indexOf(root)] + file.path.mid(root.size()));
}
bool RemoteFileDownload::openCurrent() {
    const auto& file = list_.files[index_];
    const auto path = target(file);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return fail("Could not create the folder for " + path + ".");
    auto output = std::make_unique<QSaveFile>(path);
    if (!output->open(QIODevice::WriteOnly)) return fail("Could not write " + path + ": " + output->errorString());
    file_ = std::move(output); size_ = file.size; position_ = 0;
    return true;
}
bool RemoteFileDownload::finishCurrent() {
    if (!file_->commit()) return fail("Could not save " + file_->fileName() + ": " + file_->errorString());
    file_.reset(); size_.reset(); ++copied_; ++index_;
    return true;
}
std::optional<RemoteFileDownload::Request> RemoteFileDownload::next() {
    static std::atomic<quint32> streams{0};
    if (!started_ || finished_ || failed() || outstanding_) return {};
    while (index_ < list_.files.size()) {
        const auto& file = list_.files[index_];
        if (file.directory) {
            if (!QDir().mkpath(target(file))) { fail("Could not create the folder " + target(file) + "."); return {}; }
            ++index_; continue;
        }
        if (!file_ && !openCurrent()) return {};
        if (size_ && position_ >= *size_) { if (!finishCurrent()) return {}; continue; }
        const quint32 stream = ++streams;
        outstanding_ = size_ ? Request{stream, quint32(index_), ClipboardFileContentsRange, position_, quint32(qMin<quint64>(ChunkBytes, *size_ - position_))}
                             : Request{stream, quint32(index_), ClipboardFileContentsSize, 0, 8};
        deadline_ = QDeadlineTimer(timeoutMs_);
        return outstanding_;
    }
    finished_ = true;
    return {};
}
bool RemoteFileDownload::response(quint32 stream, const uchar* bytes, qsizetype size, bool success) {
    if (!outstanding_ || outstanding_->stream != stream) return !failed();
    const auto request = *outstanding_; outstanding_.reset();
    const auto& name = list_.files[request.index].path;
    if (!success) return fail("The remote computer could not read " + name + ".");
    if (request.flags == ClipboardFileContentsSize) {
        if (size != 8 || !bytes) return fail("The remote computer returned an invalid size for " + name + ".");
        const auto value = qFromLittleEndian<quint64>(bytes);
        if (value > std::numeric_limits<quint64>::max() - list_.bytes) return fail("The remote computer returned an invalid size for " + name + ".");
        size_ = value; list_.bytes += value;
        return true;
    }
    if (!bytes || size <= 0 || quint64(size) > request.size) return fail("The remote computer returned an invalid block of " + name + ".");
    if (file_->write(reinterpret_cast<const char*>(bytes), size) != size) return fail("Could not write " + file_->fileName() + ": " + file_->errorString());
    position_ += quint64(size); done_ += quint64(size);
    return true;
}
QString RemoteFileDownload::summary() const {
    const auto files = [](int n) { return QString("%1 file%2").arg(n).arg(n == 1 ? "" : "s"); };
    if (failed()) return copied_ ? QString("%1 %2 copied before the transfer stopped.").arg(error_, files(copied_)) : error_;
    return QString("Copied %1 (%2) from the remote clipboard to %3.").arg(files(copied_), QLocale().formattedDataSize(qint64(done_)), directory_.absolutePath());
}
