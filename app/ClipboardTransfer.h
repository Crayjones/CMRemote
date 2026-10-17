#pragma once
#include <QByteArray>
#include <QDeadlineTimer>
#include <QMetaType>
#include <QString>
#include <optional>

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

// Worker-thread state. CLIPRDR has no response IDs: never replace an outstanding request.
class ClipboardProtocol final {
public:
    void control(ClipboardControl control);
    void ready();
    void reset();
    void local(QString text, ClipboardStamp stamp);
    void offer(bool unicodeText);
    std::optional<bool> advertisement();
    std::optional<ClipboardStamp> request(qint64 timeoutMs = 15000);
    qint64 responseWait() const;
    std::optional<ClipboardDelivery> response(const uchar* bytes, qsizetype size, bool success);
    const QByteArray* localReply() const;
private:
    ClipboardControl control_;
    bool ready_ = false, advertise_ = false, advertised_ = false, offered_ = false;
    quint64 offer_ = 0, requested_ = 0;
    struct Request { quint64 offer; ClipboardStamp stamp; QDeadlineTimer deadline; };
    std::optional<Request> outstanding_;
    std::optional<QByteArray> local_;
};
