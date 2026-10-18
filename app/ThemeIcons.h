#pragma once
#include <QIcon>
class QWidget;

// Dark-theme versions of the Visual Studio-style toolbar icons, made by
// swapping their fixed palette the way Visual Studio's dark theme does.
namespace ThemeIcons {
// The icon as it should look in the current theme. Icons without the
// dark-gray glyph color (full-color connection icons) come back unchanged.
QIcon themed(const QIcon& icon);
QIcon dark(const QIcon& icon);
QImage dark(const QImage& image);
// Swaps every icon on top-level windows to match the current theme.
void restyle();
}
