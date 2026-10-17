#include "RdpCursor.h"
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/codec/color.h>
#include <memory>
#include <new>

namespace {
struct CachedCursor {
    rdpPointer base;
    QImage* image;
};
BOOL createCursor(rdpContext* context, rdpPointer* pointer) {
    if (!context || !pointer || pointer->size != sizeof(CachedCursor) || !pointer->width || !pointer->height ||
        pointer->width > 384 || pointer->height > 384 || pointer->xPos >= pointer->width || pointer->yPos >= pointer->height) return FALSE;
    const auto depth = pointer->xorBpp;
    if (depth != 1 && depth != 8 && depth != 16 && depth != 24 && depth != 32) return FALSE;
    const quint64 row = depth == 1 ? (pointer->width + 7) / 8 : quint64(pointer->width) * (depth / 8);
    const quint64 xorBytes = (row + row % 2) * pointer->height;
    const quint64 maskRow = (pointer->width + 7) / 8;
    const quint64 andBytes = (maskRow + maskRow % 2) * pointer->height;
    if (!pointer->xorMaskData || pointer->lengthXorMask < xorBytes || pointer->lengthXorMask > 384 * 384 * 4 ||
        pointer->lengthAndMask > 384 * 48 || (pointer->andMaskData && pointer->lengthAndMask < andBytes) ||
        (!pointer->andMaskData && pointer->lengthAndMask) || (depth == 1 && !pointer->andMaskData)) return FALSE;
    const auto* palette = context->gdi ? &context->gdi->palette : nullptr;
    if (depth == 8 && !palette) return FALSE;
    std::unique_ptr<QImage> image(new (std::nothrow) QImage(int(pointer->width), int(pointer->height), QImage::Format_ARGB32));
    if (!image || image->isNull() || !freerdp_image_copy_from_pointer_data(image->bits(), PIXEL_FORMAT_BGRA32, image->bytesPerLine(),
        0, 0, pointer->width, pointer->height, pointer->xorMaskData, pointer->lengthXorMask,
        pointer->andMaskData, pointer->lengthAndMask, depth, palette)) return FALSE;
    auto* cached = reinterpret_cast<CachedCursor*>(pointer);
    delete cached->image; cached->image = image.release(); return TRUE;
}
void freeCursor(rdpContext*, rdpPointer* pointer) {
    auto* cached = reinterpret_cast<CachedCursor*>(pointer);
    delete cached->image; cached->image = nullptr;
}
BOOL keepPosition(rdpContext*, UINT32, UINT32) { return TRUE; }
}

rdpPointer rdpCursorPrototype(pPointer_Set set, pPointer_SetNull hide, pPointer_SetDefault reset, pPointer_SetPosition position) {
    rdpPointer pointer{}; pointer.size = sizeof(CachedCursor);
    pointer.New = createCursor; pointer.Free = freeCursor; pointer.Set = set;
    pointer.SetNull = hide; pointer.SetDefault = reset; pointer.SetPosition = position ? position : keepPosition;
    return pointer;
}
RemoteCursor rdpCursorImage(const rdpPointer* pointer) {
    if (!pointer || pointer->size != sizeof(CachedCursor)) return {};
    const auto* cached = reinterpret_cast<const CachedCursor*>(pointer);
    if (!cached->image) return {};
    return {RemoteCursor::Kind::Bitmap, *cached->image, QPoint(int(pointer->xPos), int(pointer->yPos))};
}
