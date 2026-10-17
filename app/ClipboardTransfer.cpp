#include "ClipboardTransfer.h"
#include <QtEndian>

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
void ClipboardProtocol::control(ClipboardControl next) {
    if (!next.enabled || next.stamp != control_.stamp) {
        local_.reset(); if (advertised_) advertise_ = true;
    }
    if (!next.enabled || next.stamp.permission != control_.stamp.permission || next.stamp.route != control_.stamp.route) {
        offered_ = false; ++offer_;
    }
    control_ = next;
}
void ClipboardProtocol::ready() { ready_ = true; if (control_.enabled && local_) advertise_ = true; }
void ClipboardProtocol::reset() { *this = ClipboardProtocol{}; }
void ClipboardProtocol::local(QString text, ClipboardStamp stamp) {
    if (!control_.enabled || !control_.focused || stamp != control_.stamp) return;
    auto bytes = encodeClipboardText(text);
    if (bytes == local_) return;
    local_ = std::move(bytes); advertise_ = local_.has_value() || advertised_;
}
void ClipboardProtocol::offer(bool unicodeText) {
    // The server now owns the clipboard. Withdrawing our old formats would replace its copy.
    local_.reset(); advertised_ = false; advertise_ = false;
    ++offer_; offered_ = control_.enabled && unicodeText;
}
std::optional<bool> ClipboardProtocol::advertisement() {
    if (!ready_ || !advertise_) return {};
    advertise_ = false; advertised_ = control_.enabled && local_.has_value(); return advertised_;
}
std::optional<ClipboardStamp> ClipboardProtocol::request(qint64 timeoutMs) {
    if (!ready_ || !control_.enabled || !control_.focused || !offered_ || outstanding_ || requested_ == offer_) return {};
    requested_ = offer_; outstanding_ = Request{offer_, control_.stamp, QDeadlineTimer(qBound(qint64(0), timeoutMs, qint64(15000)))}; return control_.stamp;
}
qint64 ClipboardProtocol::responseWait() const { return outstanding_ ? outstanding_->deadline.remainingTime() : -1; }
std::optional<ClipboardDelivery> ClipboardProtocol::response(const uchar* bytes, qsizetype size, bool success) {
    auto request = outstanding_; outstanding_.reset();
    if (!request || !success || !control_.enabled || request->offer != offer_ || request->stamp != control_.stamp) return {};
    auto text = decodeClipboardText(bytes, size);
    if (!text) return {};
    return ClipboardDelivery{std::move(*text), request->stamp};
}
const QByteArray* ClipboardProtocol::localReply() const {
    return ready_ && control_.enabled && control_.focused && local_ ? &*local_ : nullptr;
}
