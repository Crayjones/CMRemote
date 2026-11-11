#include "VncGraphics.h"
#include <cstring>
#include <limits>

bool VncGraphics::capture(const QRegion& damage, quint64 epoch, quint64 revision) {
    const auto now = PerformanceTrace::now();
    std::lock_guard<std::mutex> lock(updateMutex_);
    const auto result = updates_.capture(framebuffer_.constBits(), framebuffer_.size(), framebuffer_.bytesPerLine(),
        damage, {epoch, revision, now, now, 0});
    if (!result.accepted) return false;
    updatePending_ = true;
    epoch_ = epoch; revision_ = revision;
    return true;
}
uchar* VncGraphics::geometry(int w, int h) {
    if (w <= 0 || h <= 0 || w > MaxSide || h > MaxSide || quint64(w) * quint64(h) > MaxPixels) return nullptr;
    // A same-size NewFBSize keeps the pixels; the server refreshes them anyway.
    if (framebuffer_.size() == QSize(w, h)) return framebuffer_.bits();
    QImage replacement(w, h, QImage::Format_RGB32);
    if (replacement.isNull() || replacement.bytesPerLine() != w * 4) return nullptr;
    replacement.fill(qRgb(0, 0, 0));
    // New geometry is published at commit as a full snapshot of the new epoch. The
    // decoder writes through bits(); this image is never shared, so bits() never detaches.
    framebuffer_ = std::move(replacement);
    geometryPending_ = true; pending_ = QRegion(framebuffer_.rect());
    return framebuffer_.bits();
}
bool VncGraphics::commit() {
    if (pending_.isEmpty() && !geometryPending_) return true;
    if (framebuffer_.isNull() || epoch_ == std::numeric_limits<quint64>::max() ||
        revision_ == std::numeric_limits<quint64>::max()) return false;
    const auto epoch = geometryPending_ ? epoch_ + 1 : epoch_;
    const auto damage = geometryPending_ ? QRegion(framebuffer_.rect()) : pending_;
    if (!capture(damage, epoch, revision_ + 1)) return false;
    pending_ = {}; geometryPending_ = false;
    return true;
}
bool VncGraphics::damage(int x, int y, int w, int h) {
    if (framebuffer_.isNull() || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        qint64(x) + w > framebuffer_.width() || qint64(y) + h > framebuffer_.height()) return false;
    // The fourth byte is RFB padding, never transparency: make it opaque for RGB32.
    for (int row = y; row < y + h; ++row) {
        auto* pixel = reinterpret_cast<QRgb*>(framebuffer_.scanLine(row)) + x;
        for (int column = 0; column < w; ++column) pixel[column] |= 0xff000000u;
    }
    pending_ += QRect(x, y, w, h);
    return true;
}
bool VncGraphics::cursor(quint16 hotx, quint16 hoty, quint16 w, quint16 h, const uchar* bytes, size_t len) {
    if (w > 384 || h > 384) return false;
    RemoteCursor cursor;
    if (!w || !h) {
        if (len || hotx || hoty) return false;
        cursor.kind = RemoteCursor::Kind::Hidden;
    } else {
        if (hotx >= w || hoty >= h || !bytes || len != size_t(w) * h * 4) return false;
        cursor.kind = RemoteCursor::Kind::Bitmap;
        cursor.hotspot = QPoint(hotx, hoty);
        cursor.image = QImage(w, h, QImage::Format_ARGB32);
        if (cursor.image.isNull()) return false;
        for (int row = 0; row < h; ++row) {
            auto* destination = reinterpret_cast<QRgb*>(cursor.image.scanLine(row));
            const auto* source = bytes + size_t(row) * w * 4;
            for (int column = 0; column < w; ++column, source += 4)
                destination[column] = qRgba(source[2], source[1], source[0], source[3]);
        }
    }
    std::lock_guard<std::mutex> lock(cursorMutex_);
    cursors_.publish(std::move(cursor)); cursorPending_ = true;
    return true;
}
DesktopUpdate VncGraphics::takeUpdate() {
    std::lock_guard<std::mutex> lock(updateMutex_);
    updatePending_ = false;
    return updates_.take();
}
std::optional<RemoteCursor> VncGraphics::takeCursor() {
    std::lock_guard<std::mutex> lock(cursorMutex_);
    cursorPending_ = false;
    return cursors_.take();
}
bool VncGraphics::hasUpdate() const {
    std::lock_guard<std::mutex> lock(updateMutex_);
    return updatePending_;
}
bool VncGraphics::hasCursor() const {
    std::lock_guard<std::mutex> lock(cursorMutex_);
    return cursorPending_;
}
void VncGraphics::reset() {
    framebuffer_ = {}; pending_ = {}; geometryPending_ = false;
    // Retain monotonically increasing counters across reset so already-taken
    // batches cannot become current again if an instance is reused.
    {
        std::lock_guard<std::mutex> lock(updateMutex_);
        updates_.reset(); updatePending_ = false;
    }
    std::lock_guard<std::mutex> lock(cursorMutex_);
    cursors_.clear(); cursorPending_ = false;
}
