#pragma once
#include "PerformanceTrace.h"
#include <QImage>
#include <QRegion>
#include <map>
#include <vector>

struct DesktopPatch {
    QRect rectangle;
    QImage pixels;
};
struct DesktopUpdate {
    QSize size;
    FrameStamp stamp;
    bool full = false;
    std::vector<DesktopPatch> patches;
};
bool desktopSizeValid(QSize size);
// Validate the entire batch before changing the retained CPU desktop.
bool applyDesktopUpdate(QImage& desktop, const DesktopUpdate& update);

// The caller owns synchronization and captures only inside a completed GDI paint/flush.
class DesktopUpdateAccumulator final {
public:
    static constexpr int TileSize = 64;
    static constexpr size_t MaxPatches = 512;
    static constexpr int MaxRegions = 256;
    struct Capture { bool accepted = false, notify = false; quint64 copiedBytes = 0; };
    Capture capture(const uchar* bytes, QSize size, int stride, const QRegion& damage, FrameStamp stamp);
    DesktopUpdate take();
    void reset();
    size_t patchCount() const { return full_.isNull() ? patches_.size() : 1; }
    quint64 bytes() const;
private:
    QSize size_;
    FrameStamp stamp_;
    QImage full_;
    std::map<int, DesktopPatch> patches_;
    bool posted_ = false;
};
