#pragma once
#include <QByteArray>
#include <QDeadlineTimer>
#include <QMetaType>
#include <QString>
#include <map>
#include <optional>
#include <vector>
#include "ClipboardFiles.h"

struct ClipboardStamp {
    quint64 permission = 0, route = 0, host = 0;
    bool operator==(const ClipboardStamp& other) const { return permission == other.permission && route == other.route && host == other.host; }
    bool operator!=(const ClipboardStamp& other) const { return !(*this == other); }
};
struct ClipboardControl { bool enabled = false, focused = false; ClipboardStamp stamp; };
struct ClipboardDelivery { QString text; ClipboardStamp stamp; };
Q_DECLARE_METATYPE(ClipboardStamp)
Q_DECLARE_METATYPE(ClipboardDelivery)

std::optional<QByteArray> encodeClipboardText(const QString& text);
std::optional<QString> decodeClipboardText(const uchar* bytes, qsizetype size);

struct ClipboardAdvertisement {
    bool text = false, files = false;
    bool operator==(const ClipboardAdvertisement& other) const { return text == other.text && files == other.files; }
};
inline constexpr quint32 ClipboardUnicodeTextFormat = 13; // CF_UNICODETEXT
struct ClipboardRequest { quint32 format; ClipboardStamp stamp; };
// The server's current file list; id changes whenever the list is replaced or withdrawn.
// stamp: the route that fetched the list; a host copy made since then wins over it.
struct RemoteFilesUpdate { quint64 id = 0; std::shared_ptr<const RemoteFileList> files; ClipboardStamp stamp; };

// Worker-thread state. CLIPRDR format data has no response IDs: never replace an outstanding request.
class ClipboardProtocol final {
public:
    void control(ClipboardControl control);
    void ready();
    void reset();
    // Server general capability flags; CB_CAN_LOCK_CLIPDATA makes each file offer locked.
    void serverCapabilities(quint32 flags);
    void local(QString text, ClipboardStamp stamp);
    // Host file paths travel with the same stamp as text; only an authorized route builds a list.
    bool acceptsLocal(ClipboardStamp stamp) const;
    void localFiles(std::shared_ptr<const LocalFileList> files, ClipboardStamp stamp);
    void offer(bool unicodeText, std::optional<quint32> fileListFormat = {});
    std::optional<ClipboardAdvertisement> advertisement();
    std::optional<ClipboardRequest> request(qint64 timeoutMs = 15000);
    qint64 responseWait() const;
    std::optional<ClipboardDelivery> response(const uchar* bytes, qsizetype size, bool success);
    const QByteArray* localReply() const;
    const QByteArray* localFileList() const;
    // Server reads of offered host files. A locked list stays readable after the clipboard moves on;
    // the lock id is ignored for servers that did not offer locking.
    std::optional<QByteArray> fileContents(std::optional<quint32> lock, quint32 index, quint32 flags, quint64 position, quint32 requested) const;
    void lock(quint32 id);
    void unlock(quint32 id);
    std::optional<RemoteFilesUpdate> takeRemoteFiles();
    std::shared_ptr<const RemoteFileList> remoteFiles(quint64 id) const;
    // Client locks on server file lists (as Windows expects): send takeLock() before the list
    // request and takeUnlocks() whenever offered. A download pins its list's lock until done.
    std::optional<quint32> takeLock();
    std::vector<quint32> takeUnlocks();
    std::optional<quint32> remoteFilesLock(quint64 id) const;
    void pin(std::optional<quint32> lock);
    void unpin();
    bool enabled() const { return control_.enabled; }
private:
    void withdrawRemoteFiles();
    void releaseLock();
    ClipboardControl control_;
    bool ready_ = false, advertise_ = false, advertised_ = false, offered_ = false;
    quint64 offer_ = 0, requested_ = 0, fileListRequested_ = 0;
    std::optional<quint32> fileListFormat_;
    struct Request { quint64 offer; quint32 format; ClipboardStamp stamp; QDeadlineTimer deadline; };
    std::optional<Request> outstanding_;
    std::optional<QByteArray> local_;
    std::shared_ptr<const LocalFileList> localFiles_;
    std::map<quint32, std::shared_ptr<const LocalFileList>> locks_;
    std::shared_ptr<const RemoteFileList> remoteFiles_;
    ClipboardStamp remoteFilesStamp_;
    quint64 remoteFilesId_ = 0;
    bool remoteFilesChanged_ = false;
    bool serverLocks_ = false, lockPending_ = false;
    quint32 nextLock_ = 1;
    std::optional<quint32> fileLock_, pinned_;
    std::vector<quint32> unlocks_;
};
