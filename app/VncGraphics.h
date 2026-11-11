#pragma once
#include "DesktopUpdate.h"
#include "RemoteCursor.h"
#include <cstddef>
#include <mutex>

// Mutations/reset belong to one transport thread. Only the owned mailboxes are
// shared with GUI delivery; the canonical framebuffer never leaves this object.
// LibVNCClient decodes rectangles straight into framebuffer() (32-bit, B,G,R,X
// in memory: QImage::Format_RGB32 on little-endian hosts). damage() marks what
// changed and commit() publishes one complete RFB FramebufferUpdate, so the GUI
// never presents a partially applied update.
class VncGraphics final {
public:
    static constexpr int MaxSide = 8192;
    static constexpr quint64 MaxPixels = 8294400;
    // Replaces the framebuffer (black) for a new desktop size; nullptr if out of bounds.
    uchar* geometry(int w, int h);
    uchar* framebuffer() { return framebuffer_.isNull() ? nullptr : framebuffer_.bits(); }
    QSize size() const { return framebuffer_.size(); }
    bool damage(int x, int y, int w, int h);
    // Decoded cursor BGRA already contains the RFB mask in its alpha channel.
    bool cursor(quint16 hotx, quint16 hoty, quint16 w, quint16 h, const uchar* bytes, size_t len);
    bool commit();
    DesktopUpdate takeUpdate();
    std::optional<RemoteCursor> takeCursor();
    bool hasUpdate() const;
    bool hasCursor() const;
    void reset();
private:
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
