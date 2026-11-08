#pragma once
#include <QApplication>
#include <QPalette>
#include <QString>

// The whole look of the application: the Fusion style, a light or dark palette
// and app/ui/theme.qss, installed once on the application. Windows, dialogs and
// widgets never set their own stylesheet or palette; they name an element with
// an objectName and theme.qss styles it.

// The palette for the light theme or for the Visual Studio Dark look.
QPalette themePalette(bool dark);

// Fusion, the global stylesheet, dock-button glyphs and the light palette.
// Call once, right after creating the application.
void applyAppearance(QApplication& app);

// theme is "system", "light" or "dark". System follows the OS and its later changes.
QString& currentTheme();
// Explicit choices win even where the platform cannot switch its own color scheme.
bool themeIsDark();
void applyTheme(QApplication& app, const QString& theme);
