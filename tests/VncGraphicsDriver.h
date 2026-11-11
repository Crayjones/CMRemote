#pragma once
#include "VncGraphics.h"
#include <cstring>

// LibVNCClient decodes straight into the framebuffer (B,G,R,X per pixel) and then
// reports the rectangle; these helpers do the same for synthetic rectangles.
inline bool raw(VncGraphics& graphics, int x, int y, int w, int h, const uchar* bytes, size_t len) {
    auto* framebuffer = graphics.framebuffer(); const auto size = graphics.size();
    if (!framebuffer || !bytes || w <= 0 || h <= 0 || len != size_t(w) * size_t(h) * 4 || x < 0 || y < 0 ||
        qint64(x) + w > size.width() || qint64(y) + h > size.height()) return false;
    for (int row = 0; row < h; ++row)
        std::memcpy(framebuffer + (size_t(y + row) * size.width() + x) * 4, bytes + size_t(row) * w * 4, size_t(w) * 4);
    return graphics.damage(x, y, w, h);
}
// CopyRect, as LibVNCClient's CopyRectangleFromRectangle (memmove handles overlap).
inline bool copy(VncGraphics& graphics, int dstx, int dsty, int w, int h, int srcx, int srcy) {
    auto* framebuffer = graphics.framebuffer(); const auto size = graphics.size();
    const auto inside = [&](int x, int y) { return x >= 0 && y >= 0 && qint64(x) + w <= size.width() && qint64(y) + h <= size.height(); };
    if (!framebuffer || w <= 0 || h <= 0 || !inside(dstx, dsty) || !inside(srcx, srcy)) return false;
    const auto stride = size_t(size.width()) * 4;
    if (srcy < dsty) {
        for (int row = h - 1; row >= 0; --row)
            std::memmove(framebuffer + size_t(dsty + row) * stride + size_t(dstx) * 4, framebuffer + size_t(srcy + row) * stride + size_t(srcx) * 4, size_t(w) * 4);
    } else {
        for (int row = 0; row < h; ++row)
            std::memmove(framebuffer + size_t(dsty + row) * stride + size_t(dstx) * 4, framebuffer + size_t(srcy + row) * stride + size_t(srcx) * 4, size_t(w) * 4);
    }
    return graphics.damage(dstx, dsty, w, h);
}
