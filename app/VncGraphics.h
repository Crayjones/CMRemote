#pragma once
#include "DesktopUpdate.h"
#include "RemoteCursor.h"
#include <cstddef>
#include <mutex>

// Mutations/reset belong to one transport thread. Only the owned mailboxes are
// shared with GUI delivery; the canonical framebuffer never leaves this object.
// Rectangles mutate the framebuffer only; commit() publishes one complete RFB
// FramebufferUpdate so the GUI never presents a partially applied update.
class VncGraphics final {
public:
    static constexpr int MaxSide = 8192;
    static constexpr quint64 MaxPixels = 8294400;
    bool geometry(quint16 w, quint16 h);
    bool raw(quint16 x, quint16 y, quint16 w, quint16 h, const uchar* bytes, size_t len);
    bool copy(quint16 dstx, quint16 dsty, quint16 w, quint16 h, quint16 srcx, quint16 srcy);
    // Decoded cursor BGRA already contains the RFB mask in its alpha channel.
    bool cursor(quint16 hotx, quint16 hoty, quint16 w, quint16 h, const uchar* bytes, size_t len);
    bool commit();
    DesktopUpdate takeUpdate();
    std::optional<RemoteCursor> takeCursor();
    bool hasUpdate() const;
    bool hasCursor() const;
    void reset();
private:
    bool rectangleValid(quint16 x, quint16 y, quint16 w, quint16 h) const;
    bool capture(const QRegion& damage, quint64 epoch, quint64 revision);
    QImage framebuffer_;
    quint64 epoch_ = 0, revision_ = 0;
    QRegion pending_;
    bool geometryPending_ = false;
    mutable std::mutex updateMutex_, cursorMutex_;
    DesktopUpdateAccumulator updates_;
    RemoteCursorMailbox cursors_;
    bool updatePending_ = false, cursorPending_ = false;
};
