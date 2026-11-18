#include "ClipboardTransfer.h"
#include <QtEndian>
#include <utility>

namespace {
constexpr qsizetype MaxWire = 1024 * 1024;
bool validText(const QString& text) {
    for (qsizetype i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        if (c.isNull() || c.isLowSurrogate()) return false;
        if (c.isHighSurrogate() && (++i == text.size() || !text[i].isLowSurrogate())) return false;
    }
    return true;
}
}
std::optional<QByteArray> encodeClipboardText(const QString& text) {
    if (text.size() > MaxWire / 2 - 1 || !validText(text)) return {};
    QString normalized; normalized.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            normalized += "\r\n";
        } else if (text[i] == '\n') normalized += "\r\n";
        else normalized += text[i];
        if (normalized.size() > MaxWire / 2 - 1) return {};
    }
    QByteArray bytes((normalized.size() + 1) * 2, '\0');
    for (qsizetype i = 0; i < normalized.size(); ++i)
        qToLittleEndian<quint16>(normalized[i].unicode(), reinterpret_cast<uchar*>(bytes.data()) + i * 2);
    return bytes;
}
std::optional<QString> decodeClipboardText(const uchar* bytes, qsizetype size) {
    if (!bytes || size < 2 || size > MaxWire || size % 2 || qFromLittleEndian<quint16>(bytes + size - 2)) return {};
    QString text; text.reserve(size / 2 - 1);
    for (qsizetype i = 0; i < size - 2; i += 2) text += QChar(qFromLittleEndian<quint16>(bytes + i));
    if (!validText(text)) return {};
    text.replace("\r\n", "\n"); text.replace('\r', '\n'); return text;
}
namespace {
bool sameFiles(const LocalFileList* a, const LocalFileList* b) {
    if (!a || !b) return a == b;
    if (a->descriptor != b->descriptor || a->files.size() != b->files.size()) return false;
    for (size_t i = 0; i < a->files.size(); ++i) if (a->files[i].path != b->files[i].path) return false;
    return true;
}
}
void ClipboardProtocol::control(ClipboardControl next) {
    if (!next.enabled || next.stamp != control_.stamp) {
        local_.reset(); localFiles_.reset(); if (advertised_) advertise_ = true;
    }
    if (!next.enabled || next.stamp.permission != control_.stamp.permission || next.stamp.route != control_.stamp.route) {
        offered_ = false; fileListFormat_.reset(); ++offer_;
    }
    if (!next.enabled) { locks_.clear(); withdrawRemoteFiles(); releaseLock(); }
    control_ = next;
}
void ClipboardProtocol::serverCapabilities(quint32 flags) { serverLocks_ = flags & 0x10; } // CB_CAN_LOCK_CLIPDATA
void ClipboardProtocol::ready() { ready_ = true; if (control_.enabled && (local_ || localFiles_)) advertise_ = true; }
void ClipboardProtocol::reset() {
    // List ids keep increasing so a download chosen before a reconnect never matches a new list.
    // Locks die with the channel; nothing is left to unlock.
    const auto id = remoteFilesId_ + 1; const bool changed = remoteFiles_ || remoteFilesChanged_; const auto lock = nextLock_;
    *this = ClipboardProtocol{}; remoteFilesId_ = id; remoteFilesChanged_ = changed; nextLock_ = lock;
}
void ClipboardProtocol::local(QString text, ClipboardStamp stamp) {
    if (!acceptsLocal(stamp)) return;
    auto bytes = encodeClipboardText(text);
    if (bytes == local_) return;
    local_ = std::move(bytes); advertise_ = local_.has_value() || advertised_;
}
bool ClipboardProtocol::acceptsLocal(ClipboardStamp stamp) const { return control_.enabled && control_.focused && stamp == control_.stamp; }
void ClipboardProtocol::localFiles(std::shared_ptr<const LocalFileList> files, ClipboardStamp stamp) {
    if (!acceptsLocal(stamp) || sameFiles(files.get(), localFiles_.get())) return;
    localFiles_ = std::move(files); advertise_ = localFiles_ || advertised_;
}
void ClipboardProtocol::offer(bool unicodeText, std::optional<quint32> fileListFormat) {
    // The server now owns the clipboard. Withdrawing our old formats would replace its copy.
    local_.reset(); localFiles_.reset(); advertised_ = false; advertise_ = false;
    ++offer_; offered_ = control_.enabled && unicodeText;
    fileListFormat_.reset();
    if (control_.enabled && fileListFormat && *fileListFormat != ClipboardUnicodeTextFormat) fileListFormat_ = fileListFormat;
    withdrawRemoteFiles(); releaseLock();
    if (fileListFormat_ && serverLocks_) { fileLock_ = nextLock_++; if (!nextLock_) nextLock_ = 1; lockPending_ = true; }
}
void ClipboardProtocol::releaseLock() {
    if (fileLock_ && !lockPending_ && fileLock_ != pinned_) unlocks_.push_back(*fileLock_);
    fileLock_.reset(); lockPending_ = false;
}
std::optional<quint32> ClipboardProtocol::takeLock() {
    // Sent just before the list request, so a lock is never left for an offer never fetched.
    if (!lockPending_ || !fileLock_ || !ready_ || !control_.enabled || !control_.focused || outstanding_) return {};
    lockPending_ = false; return fileLock_;
}
std::vector<quint32> ClipboardProtocol::takeUnlocks() { return std::exchange(unlocks_, {}); }
std::optional<quint32> ClipboardProtocol::remoteFilesLock(quint64 id) const {
    return remoteFiles(id) ? fileLock_ : std::nullopt;
}
void ClipboardProtocol::pin(std::optional<quint32> lock) { pinned_ = lock; }
void ClipboardProtocol::unpin() {
    if (pinned_ && pinned_ != fileLock_) unlocks_.push_back(*pinned_);
    pinned_.reset();
}
std::optional<ClipboardAdvertisement> ClipboardProtocol::advertisement() {
    if (!ready_ || !advertise_) return {};
    advertise_ = false;
    const ClipboardAdvertisement formats{control_.enabled && local_.has_value(), control_.enabled && localFiles_ != nullptr};
    advertised_ = formats.text || formats.files; return formats;
}
std::optional<ClipboardRequest> ClipboardProtocol::request(qint64 timeoutMs) {
    if (!ready_ || !control_.enabled || !control_.focused || outstanding_) return {};
    quint32 format;
    if (offered_ && requested_ != offer_) { requested_ = offer_; format = ClipboardUnicodeTextFormat; }
    else if (fileListFormat_ && fileListRequested_ != offer_ && !lockPending_) { fileListRequested_ = offer_; format = *fileListFormat_; }
    else return {};
    outstanding_ = Request{offer_, format, control_.stamp, QDeadlineTimer(qBound(qint64(0), timeoutMs, qint64(15000)))};
    return ClipboardRequest{format, control_.stamp};
}
qint64 ClipboardProtocol::responseWait() const { return outstanding_ ? outstanding_->deadline.remainingTime() : -1; }
std::optional<ClipboardDelivery> ClipboardProtocol::response(const uchar* bytes, qsizetype size, bool success) {
    auto request = outstanding_; outstanding_.reset();
    if (!request || !success || !control_.enabled || request->offer != offer_) return {};
    if (request->format != ClipboardUnicodeTextFormat) {
        auto files = parseRemoteFileList(bytes, size);
        if (files && !files->files.empty()) {
            remoteFiles_ = std::make_shared<const RemoteFileList>(std::move(*files)); ++remoteFilesId_; remoteFilesChanged_ = true;
            remoteFilesStamp_ = request->stamp;
        }
        return {};
    }
    if (request->stamp != control_.stamp) return {};
    auto text = decodeClipboardText(bytes, size);
    if (!text) return {};
    return ClipboardDelivery{std::move(*text), request->stamp};
}
const QByteArray* ClipboardProtocol::localReply() const {
    return ready_ && control_.enabled && control_.focused && local_ ? &*local_ : nullptr;
}
const QByteArray* ClipboardProtocol::localFileList() const {
    return ready_ && control_.enabled && control_.focused && localFiles_ ? &localFiles_->descriptor : nullptr;
}
std::optional<QByteArray> ClipboardProtocol::fileContents(std::optional<quint32> lock, quint32 index, quint32 flags, quint64 position, quint32 requested) const {
    if (!ready_ || !control_.enabled) return {};
    const LocalFileList* list = localFiles_.get();
    // Servers without CB_CAN_LOCK_CLIPDATA never lock; xrdp still sends clipDataId 0 for the current list.
    if (lock && serverLocks_) { const auto it = locks_.find(*lock); list = it == locks_.end() ? nullptr : it->second.get(); }
    if (!list) return {};
    return readLocalFileContents(*list, index, flags, position, requested);
}
void ClipboardProtocol::lock(quint32 id) {
    if (!control_.enabled || !localFiles_) return;
    if (locks_.size() >= 64 && !locks_.count(id)) locks_.erase(locks_.begin());
    locks_[id] = localFiles_;
}
void ClipboardProtocol::unlock(quint32 id) { locks_.erase(id); }
void ClipboardProtocol::withdrawRemoteFiles() {
    if (!remoteFiles_) return;
    remoteFiles_.reset(); ++remoteFilesId_; remoteFilesChanged_ = true;
}
std::optional<RemoteFilesUpdate> ClipboardProtocol::takeRemoteFiles() {
    if (!remoteFilesChanged_) return {};
    remoteFilesChanged_ = false; return RemoteFilesUpdate{remoteFilesId_, remoteFiles_, remoteFilesStamp_};
}
std::shared_ptr<const RemoteFileList> ClipboardProtocol::remoteFiles(quint64 id) const {
    return control_.enabled && id == remoteFilesId_ ? remoteFiles_ : nullptr;
}
