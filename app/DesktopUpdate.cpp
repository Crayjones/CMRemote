#include "DesktopUpdate.h"
#include <cstring>
#include <limits>
#include <set>

bool desktopSizeValid(QSize size) {
    return size.width() > 0 && size.height() > 0 && size.width() <= 8192 && size.height() <= 8192 &&
        quint64(size.width()) * size.height() <= 16 * 1024 * 1024;
}
bool applyDesktopUpdate(QImage& desktop, const DesktopUpdate& update) {
    if (!desktopSizeValid(update.size) || update.patches.empty() ||
        update.patches.size() > DesktopUpdateAccumulator::MaxPatches) return false;
    const QRect bounds(QPoint(0, 0), update.size);
    if (update.full && (update.patches.size() != 1 || update.patches.front().rectangle != bounds)) return false;
    if (!update.full && (desktop.size() != update.size || desktop.format() != QImage::Format_RGB32)) return false;
    quint64 pixels = 0;
    for (const auto& patch : update.patches) {
        if (patch.rectangle.isEmpty() || !bounds.contains(patch.rectangle) ||
            patch.pixels.size() != patch.rectangle.size() || patch.pixels.format() != QImage::Format_RGB32) return false;
        pixels += quint64(patch.rectangle.width()) * patch.rectangle.height();
        if (pixels > quint64(update.size.width()) * update.size.height()) return false;
    }
    if (update.full) { desktop = update.patches.front().pixels; return true; }
    // No submitted CPU snapshot is retained internally. External frame() snapshots
    // still get QImage's normal copy-on-write protection.
    auto* destination = desktop.bits();
    for (const auto& patch : update.patches) for (int y = 0; y < patch.rectangle.height(); ++y)
        std::memcpy(destination + qsizetype(y + patch.rectangle.y()) * desktop.bytesPerLine() + patch.rectangle.x() * 4,
            patch.pixels.constScanLine(y), size_t(patch.rectangle.width()) * 4);
    return true;
}
DesktopUpdateAccumulator::Capture DesktopUpdateAccumulator::capture(const uchar* bytes, QSize size, int stride,
    const QRegion& damage, FrameStamp frame) {
    if (!bytes || !desktopSizeValid(size) || stride < size.width() * 4 || stride > size.width() * 4 + 4096 ||
        !frame.geometry || !frame.revision || frame.geometry < stamp_.geometry ||
        (frame.geometry == stamp_.geometry && (frame.revision <= stamp_.revision || size != size_))) return {};
    const QRect bounds(QPoint(0, 0), size);
    const bool changed = frame.geometry != stamp_.geometry;
    QRegion clipped = damage.rectCount() > MaxRegions ? QRegion(bounds) : damage.intersected(bounds);
    if (clipped.isEmpty() && !changed) return {true, false, 0};
    bool snapshot = changed || clipped == QRegion(bounds);
    std::set<int> tiles;
    const int columns = (size.width() + TileSize - 1) / TileSize;
    if (!snapshot && full_.isNull()) {
        for (const auto& rectangle : clipped) {
            for (int y = rectangle.top() / TileSize; y <= rectangle.bottom() / TileSize && !snapshot; ++y)
                for (int x = rectangle.left() / TileSize; x <= rectangle.right() / TileSize; ++x) {
                    tiles.insert(y * columns + x);
                    if (tiles.size() > MaxPatches) { snapshot = true; break; }
                }
            if (snapshot) break;
        }
        size_t count = patches_.size();
        for (const auto tile : tiles) if (!patches_.count(tile)) ++count;
        snapshot |= count > MaxPatches || quint64(count) * TileSize * TileSize >= quint64(size.width()) * size.height() / 2;
    }
    Capture result{true, !posted_, 0};
    if (snapshot) {
        auto image = QImage(bytes, size.width(), size.height(), stride, QImage::Format_RGB32).copy();
        if (image.isNull()) return {};
        result.copiedBytes = quint64(size.width()) * size.height() * 4;
        full_ = std::move(image); patches_.clear();
    } else if (!full_.isNull()) {
        // A queued full recovery already owns all pixels. Update only its dirty
        // rows instead of copying the whole desktop again while the GUI is busy.
        auto* destination = full_.bits();
        for (const auto& rectangle : clipped) {
            for (int y = rectangle.top(); y <= rectangle.bottom(); ++y)
                std::memcpy(destination + qsizetype(y) * full_.bytesPerLine() + rectangle.x() * 4,
                    bytes + qsizetype(y) * stride + rectangle.x() * 4, size_t(rectangle.width()) * 4);
            result.copiedBytes += quint64(rectangle.width()) * rectangle.height() * 4;
        }
    } else {
        std::map<int, DesktopPatch> incoming;
        const QImage borrowed(bytes, size.width(), size.height(), stride, QImage::Format_RGB32);
        for (const auto tile : tiles) {
            const QRect rectangle = QRect((tile % columns) * TileSize, (tile / columns) * TileSize, TileSize, TileSize).intersected(bounds);
            auto pixels = borrowed.copy(rectangle);
            if (pixels.isNull()) return {};
            result.copiedBytes += quint64(rectangle.width()) * rectangle.height() * 4;
            incoming.emplace(tile, DesktopPatch{rectangle, std::move(pixels)});
        }
        for (auto& entry : incoming) patches_.insert_or_assign(entry.first, std::move(entry.second));
    }
    if (posted_ && !changed) {
        frame.oldestNs = stamp_.oldestNs;
        frame.replacements = stamp_.replacements == std::numeric_limits<quint64>::max() ? stamp_.replacements : stamp_.replacements + 1;
    }
    size_ = size; stamp_ = frame; posted_ = true;
    return result;
}
DesktopUpdate DesktopUpdateAccumulator::take() {
    if (!posted_) return {};
    DesktopUpdate update{size_, stamp_, !full_.isNull(), {}};
    if (update.full) update.patches.push_back({QRect(QPoint(0, 0), size_), std::move(full_)});
    else {
        update.patches.reserve(patches_.size());
        for (auto& entry : patches_) update.patches.push_back(std::move(entry.second));
    }
    patches_.clear(); posted_ = false;
    return update;
}
void DesktopUpdateAccumulator::reset() { size_ = {}; stamp_ = {}; full_ = {}; patches_.clear(); posted_ = false; }
quint64 DesktopUpdateAccumulator::bytes() const {
    quint64 total = quint64(full_.sizeInBytes());
    for (const auto& entry : patches_) total += quint64(entry.second.pixels.sizeInBytes());
    return total;
}
