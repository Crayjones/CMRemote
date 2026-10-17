#pragma once
#include <QImage>
#include <QPoint>
#include <QtMath>
#include <mutex>
#include <optional>
#include <utility>

struct RemoteCursor {
    enum class Kind { Default, Hidden, Bitmap };
    Kind kind = Kind::Default;
    QImage image;
    QPoint hotspot;
    bool valid() const {
        if (kind == Kind::Default || kind == Kind::Hidden) return image.isNull();
        return kind == Kind::Bitmap && !image.isNull() && image.width() <= 384 && image.height() <= 384 && image.rect().contains(hotspot);
    }
};

struct ScaledRemoteCursor { QImage image; QPoint hotspot; };
inline ScaledRemoteCursor scaleRemoteCursor(const RemoteCursor& cursor, QSize desktop, QSize presentation, qreal dpr) {
    if (cursor.kind != RemoteCursor::Kind::Bitmap || !cursor.valid() || desktop.isEmpty() || presentation.isEmpty() || dpr <= 0) return {};
    const qreal scale = qMin(qreal(presentation.width()) / desktop.width(), qreal(presentation.height()) / desktop.height());
    const qreal physical = qMin(scale * dpr, qreal(1024) / qMax(cursor.image.width(), cursor.image.height()));
    const QSize size(qMax(1, qRound(cursor.image.width() * physical)), qMax(1, qRound(cursor.image.height() * physical)));
    auto image = cursor.image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation); image.setDevicePixelRatio(dpr);
    const QPoint hotspot(qBound(0, qRound(cursor.hotspot.x() * physical / dpr), qMax(0, qCeil(size.width() / dpr) - 1)),
                         qBound(0, qRound(cursor.hotspot.y() * physical / dpr), qMax(0, qCeil(size.height() / dpr) - 1)));
    return {std::move(image), hotspot};
}

class RemoteCursorMailbox final {
public:
    bool publish(RemoteCursor cursor) {
        if (!cursor.valid()) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        const bool notify = !pending_;
        pending_ = std::move(cursor); return notify;
    }
    std::optional<RemoteCursor> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto cursor = std::move(pending_); pending_.reset(); return cursor;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_); pending_.reset();
    }
private:
    std::mutex mutex_;
    std::optional<RemoteCursor> pending_;
};
