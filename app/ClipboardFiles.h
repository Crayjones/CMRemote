#pragma once
#include <QByteArray>
#include <QDeadlineTimer>
#include <QDir>
#include <QSaveFile>
#include <QString>
#include <QStringList>
#include <memory>
#include <optional>
#include <vector>

// CLIPRDR file streams (MS-RDPECLIP FileGroupDescriptorW + FileContents). Qt-only: the worker
// owns the channel calls; these types build, parse and bound what crosses it.
inline constexpr char ClipboardFileListName[] = "FileGroupDescriptorW";
inline constexpr quint32 ClipboardFileListFormatId = 0xC0E0; // registered range; servers match by name
inline constexpr quint32 ClipboardFileContentsSize = 0x1, ClipboardFileContentsRange = 0x2;
inline constexpr int MaxClipboardFiles = 10000;

struct LocalClipboardFile { QString path; quint64 size = 0; bool directory = false; };
// Host files the session offers. Folders are expanded parents first; symbolic links inside
// folders are skipped so a copy cannot loop or leave the chosen tree.
struct LocalFileList {
    std::vector<LocalClipboardFile> files;
    QByteArray descriptor; // CLIPRDR_FILELIST wire bytes
};
std::shared_ptr<const LocalFileList> buildLocalFileList(const QStringList& paths, QString* error = nullptr);
// FILECONTENTS_SIZE answers the 8-byte size; FILECONTENTS_RANGE reads at most `requested` bytes.
std::optional<QByteArray> readLocalFileContents(const LocalFileList& list, quint32 index, quint32 flags, quint64 position, quint32 requested);

struct RemoteClipboardFile { QString path; bool directory = false; std::optional<quint64> size; };
struct RemoteFileList { std::vector<RemoteClipboardFile> files; quint64 bytes = 0; };
// Rejects absolute, parent-relative, duplicate or otherwise unsafe names; never trims them.
std::optional<RemoteFileList> parseRemoteFileList(const uchar* bytes, qsizetype size);

// One remote → host copy into an existing folder, one FileContents request at a time.
// Partial files are written beside their target and renamed when complete; failure or
// cancellation removes only the unfinished file.
class RemoteFileDownload final {
public:
    struct Request { quint32 stream, index, flags; quint64 position; quint32 size; };
    RemoteFileDownload(RemoteFileList list, QString directory, qint64 timeoutMs = 30000);
    ~RemoteFileDownload();
    RemoteFileDownload(const RemoteFileDownload&) = delete;
    RemoteFileDownload& operator=(const RemoteFileDownload&) = delete;
    bool start();
    std::optional<Request> next();
    // False when the response failed the transfer; responses for other streams are ignored.
    bool response(quint32 stream, const uchar* bytes, qsizetype size, bool success);
    void cancel(const QString& reason);
    bool finished() const { return finished_; }
    bool failed() const { return !error_.isEmpty(); }
    QString error() const { return error_; }
    QString summary() const;
    quint64 done() const { return done_; }
    quint64 total() const { return list_.bytes; }
    qint64 responseWait() const { return outstanding_ ? deadline_.remainingTime() : -1; }
private:
    bool fail(const QString& reason);
    bool openCurrent();
    bool finishCurrent();
    QString target(const RemoteClipboardFile& file) const;
    RemoteFileList list_;
    QDir directory_;
    QStringList roots_;    // top-level names as the server sent them
    QStringList renamed_;  // their collision-free local names
    qint64 timeoutMs_;
    size_t index_ = 0;
    quint64 position_ = 0, done_ = 0;
    std::optional<quint64> size_;
    std::unique_ptr<QSaveFile> file_;
    std::optional<Request> outstanding_;
    QDeadlineTimer deadline_;
    int copied_ = 0;
    bool started_ = false, finished_ = false;
    QString error_;
};
