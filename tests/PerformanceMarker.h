#pragma once
#include <QtGlobal>

// Test-only RGB marker: all nine bits must be valid, not an initial white Tk window.
inline int performanceMarker(const uchar* bytes, int width, int height, int stride) {
    if (!bytes || width < 72 || height <= 4 || width > 8192 || stride < width * 4) return -1;
    int marker = 0;
    for (int i = 0; i < 9; ++i) {
        const auto* p = bytes + size_t(4) * size_t(stride) + (4 + i * 8) * 4;
        if (p[0] != p[1] || p[1] != p[2] || (p[0] != 0 && p[0] != 255)) return -1;
        if (p[0] == 255) marker |= 1 << i;
    }
    return marker;
}
