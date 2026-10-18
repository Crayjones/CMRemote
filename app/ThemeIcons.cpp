#include "ThemeIcons.h"
#include "Appearance.h"
#include <QAbstractButton>
#include <QAction>
#include <QComboBox>
#include <QHash>
#include <QListWidget>
#include <QTabBar>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

namespace ThemeIcons {
namespace {
struct Swap { QRgb from, to; };
// Dark accents that disappear on a dark background; folder yellow and bright accents read on both.
constexpr Swap accents[] = {{0x00539c, 0x3794ff}, {0xa1260d, 0xf48771}, {0x388a34, 0x89d185}};
bool near(QRgb a, QRgb b) { return std::abs(qRed(a) - qRed(b)) <= 3 && std::abs(qGreen(a) - qGreen(b)) <= 3 && std::abs(qBlue(a) - qBlue(b)) <= 3; }
// Gray level of a neutral pixel, or -1 for a colored one.
int gray(QRgb pixel) {
    const int high = std::max({qRed(pixel), qGreen(pixel), qBlue(pixel)}), low = std::min({qRed(pixel), qGreen(pixel), qBlue(pixel)});
    return high - low <= 6 ? (high + low) / 2 : -1;
}
// Visual Studio-style icons: a dark-gray glyph inside a #f6f6f6 outline, or a small all-gray shape (the status dot).
bool vsStyle(const QImage& image) {
    int glyph = 0, outline = 0, opaque = 0; bool colored = false;
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) < 200) continue;
            ++opaque; const int level = gray(line[x]);
            if (level < 0) colored = true;
            else if (level >= 0x30 && level <= 0x60) ++glyph;
            else if (near(line[x], 0xf6f6f6)) ++outline;
        }
    }
    // Gray connection icons fill their square; the status dot does not.
    return glyph >= 6 && (outline >= 4 || (!colored && opaque * 2 < image.width() * image.height()));
}
}
namespace {
// Both directions, so restyle() can tell a dark copy from an original.
QHash<qint64, QIcon>& darkOf() { static QHash<qint64, QIcon> map; return map; }
QHash<qint64, QIcon>& lightOf() { static QHash<qint64, QIcon> map; return map; }
}
QImage dark(const QImage& source) {
    auto image = source.convertToFormat(QImage::Format_ARGB32);
    if (!vsStyle(image)) return source;
    for (int y = 0; y < image.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (!qAlpha(line[x])) continue;
            const int level = gray(line[x]); int to = -1;
            // Dark and mid glyph grays turn light (#424242 → #f6f6f6), light outlines and fills turn dark (#f6f6f6 → #252525).
            if (level >= 0 && level <= 0x9c) to = std::min(255, 312 - level);
            else if (level >= 0xe8) to = 0x25 + (0xf6 - std::min(level, 0xf6));
            if (to >= 0) { line[x] = qRgba(to, to, to, qAlpha(line[x])); continue; }
            for (const auto& swap : accents) if (near(line[x], swap.from)) { line[x] = (swap.to & 0xffffff) | (line[x] & 0xff000000); break; }
        }
    }
    return image;
}
QIcon dark(const QIcon& icon) {
    if (icon.isNull()) return icon;
    if (lightOf().contains(icon.cacheKey())) return icon;
    if (const auto found = darkOf().constFind(icon.cacheKey()); found != darkOf().cend()) return *found;
    auto sizes = icon.availableSizes(); if (sizes.isEmpty()) sizes = {QSize(16, 16), QSize(32, 32)};
    QIcon result; bool changed = false;
    for (const auto size : sizes) {
        const auto image = icon.pixmap(size).toImage(); const auto swapped = dark(image);
        changed = changed || swapped != image; result.addPixmap(QPixmap::fromImage(swapped));
    }
    if (!changed) result = icon;
    darkOf().insert(icon.cacheKey(), result);
    if (changed) lightOf().insert(result.cacheKey(), icon);
    return result;
}
QIcon themed(const QIcon& icon) {
    if (themeIsDark()) return dark(icon);
    if (const auto found = lightOf().constFind(icon.cacheKey()); found != lightOf().cend()) return *found;
    return icon;
}
void restyle() {
    for (auto* window : QApplication::topLevelWidgets()) {
        for (auto* action : window->findChildren<QAction*>()) if (!action->icon().isNull()) action->setIcon(themed(action->icon()));
        for (auto* button : window->findChildren<QAbstractButton*>()) if (!button->icon().isNull()) button->setIcon(themed(button->icon()));
        for (auto* tabs : window->findChildren<QTabBar*>()) for (int i = 0; i < tabs->count(); ++i) tabs->setTabIcon(i, themed(tabs->tabIcon(i)));
        for (auto* combo : window->findChildren<QComboBox*>()) for (int i = 0; i < combo->count(); ++i) combo->setItemIcon(i, themed(combo->itemIcon(i)));
        for (auto* list : window->findChildren<QListWidget*>()) for (int i = 0; i < list->count(); ++i) list->item(i)->setIcon(themed(list->item(i)->icon()));
        for (auto* tree : window->findChildren<QTreeWidget*>())
            for (QTreeWidgetItemIterator it(tree); *it; ++it) for (int column = 0; column < tree->columnCount(); ++column)
                if (const auto icon = (*it)->icon(column); !icon.isNull()) (*it)->setIcon(column, themed(icon));
    }
}
}
