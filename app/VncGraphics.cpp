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
bool VncGraphics::geometry(quint16 w, quint16 h) {
    if (!w || !h || w > MaxSide || h > MaxSide || quint64(w) * h > MaxPixels) return false;
    if (framebuffer_.size() == QSize(w, h)) return true;
    QImage replacement(w, h, QImage::Format_RGB32);
    if (replacement.isNull()) return false;
    replacement.fill(qRgb(0, 0, 0));
    // New geometry is published at commit as a full snapshot of the new epoch.
    framebuffer_ = std::move(replacement);
    geometryPending_ = true; pending_ = QRegion(framebuffer_.rect());
    return true;
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
bool VncGraphics::rectangleValid(quint16 x, quint16 y, quint16 w, quint16 h) const {
    return !framebuffer_.isNull() && w && h && quint32(x) + w <= quint32(framebuffer_.width()) &&
        quint32(y) + h <= quint32(framebuffer_.height());
}
bool VncGraphics::raw(quint16 x, quint16 y, quint16 w, quint16 h, const uchar* bytes, size_t len) {
    if (!rectangleValid(x, y, w, h) || !bytes || len != size_t(w) * h * 4) return false;
    // Numeric QRgb construction is native-endian safe. The fourth wire byte is
    // unused desktop padding, never transparency (unlike decoded cursor alpha).
    for (int row = 0; row < h; ++row) {
        auto* destination = reinterpret_cast<QRgb*>(framebuffer_.scanLine(y + row)) + x;
        const auto* source = bytes + size_t(row) * w * 4;
        for (int column = 0; column < w; ++column, source += 4)
            destination[column] = qRgb(source[2], source[1], source[0]);
    }
    pending_ += QRect(x, y, w, h);
    return true;
}
bool VncGraphics::copy(quint16 dstx, quint16 dsty, quint16 w, quint16 h, quint16 srcx, quint16 srcy) {
    if (!rectangleValid(dstx, dsty, w, h) || !rectangleValid(srcx, srcy, w, h)) return false;
    const auto source = framebuffer_.copy(QRect(srcx, srcy, w, h));
    if (source.isNull()) return false;
    for (int row = 0; row < h; ++row)
        std::memcpy(framebuffer_.scanLine(dsty + row) + size_t(dstx) * 4, source.constScanLine(row), size_t(w) * 4);
    pending_ += QRect(dstx, dsty, w, h);
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
