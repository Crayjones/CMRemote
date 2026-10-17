#pragma once
#include <QColor>
#include <QImage>
#include <algorithm>
#include <cmath>

inline QColor linearPixel(const QImage& image, double x, double y) {
    x = std::clamp(x, 0.0, double(image.width() - 1));
    y = std::clamp(y, 0.0, double(image.height() - 1));
    const int left = int(std::floor(x)), top = int(std::floor(y));
    const int right = std::min(left + 1, image.width() - 1), bottom = std::min(top + 1, image.height() - 1);
    const double tx = x - left, ty = y - top;
    const QColor a = image.pixelColor(left, top), b = image.pixelColor(right, top);
    const QColor c = image.pixelColor(left, bottom), d = image.pixelColor(right, bottom);
    auto channel = [&](int av, int bv, int cv, int dv) {
        return qRound((av * (1 - tx) + bv * tx) * (1 - ty) + (cv * (1 - tx) + dv * tx) * ty);
    };
    return {channel(a.red(), b.red(), c.red(), d.red()), channel(a.green(), b.green(), c.green(), d.green()),
            channel(a.blue(), b.blue(), c.blue(), d.blue())};
}
