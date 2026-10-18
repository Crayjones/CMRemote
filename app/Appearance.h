#pragma once
#include <QApplication>
#include <QDockWidget>
#include <QPainter>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleHints>

// Fusion with white float/close glyphs on the blue dock title bars.
class AppStyle : public QProxyStyle {
    // A white question mark on blue: the system one is dark gray and vanishes on dark dialogs.
    static QIcon questionIcon() {
        QIcon result;
        for (const int size : {32, 64, 128}) {
            QPixmap pixmap(size, size); pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen); painter.setBrush(QColor("#0078d7")); painter.drawEllipse(QRectF(1, 1, size - 2, size - 2));
            auto font = painter.font(); font.setBold(true); font.setPixelSize(int(size * 0.68)); painter.setFont(font);
            painter.setPen(Qt::white); painter.drawText(QRect(0, 0, size, size), Qt::AlignCenter, "?");
            painter.end(); result.addPixmap(pixmap);
        }
        return result;
    }
public:
    AppStyle() : QProxyStyle("Fusion") {}
    QIcon standardIcon(StandardPixmap icon, const QStyleOption* option, const QWidget* widget) const override {
        if (icon == SP_MessageBoxQuestion) return questionIcon();
        if (!qobject_cast<const QDockWidget*>(widget) || (icon != SP_TitleBarCloseButton && icon != SP_TitleBarNormalButton))
            return QProxyStyle::standardIcon(icon, option, widget);
        QIcon result;
        for (const qreal scale : {1.0, 2.0}) {
            QPixmap pixmap(QSize(10, 10) * scale); pixmap.setDevicePixelRatio(scale); pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(Qt::white, 1.2, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
            if (icon == SP_TitleBarCloseButton) { painter.drawLine(QPointF(1.5, 1.5), QPointF(8.5, 8.5)); painter.drawLine(QPointF(8.5, 1.5), QPointF(1.5, 8.5)); }
            else { painter.drawRect(QRectF(1.5, 3.5, 5, 5)); painter.drawPolyline(QPolygonF{{3.5, 3.5}, {3.5, 1.5}, {8.5, 1.5}, {8.5, 6.5}, {6.5, 6.5}}); }
            painter.end(); result.addPixmap(pixmap);
        }
        return result;
    }
};

// Light matches the classic Windows look; dark keeps the same layout on gray surfaces.
inline QPalette themePalette(bool dark) {
    if (!dark) {
        QPalette palette;
        palette.setColor(QPalette::Window, QColor("#f0f0f0"));
        palette.setColor(QPalette::WindowText, QColor("#202020"));
        palette.setColor(QPalette::Base, Qt::white);
        palette.setColor(QPalette::Text, QColor("#202020"));
        palette.setColor(QPalette::Button, QColor("#f0f0f0"));
        palette.setColor(QPalette::ButtonText, QColor("#202020"));
        palette.setColor(QPalette::Highlight, QColor("#0078d7"));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#808080"));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#808080"));
        return palette;
    }
    QPalette palette(QColor("#3c3c3c"), QColor("#2b2b2b")); // button, window: derives the bevel shades
    const QColor text("#e6e6e6"), disabled("#7a7a7a");
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, QColor("#1e1e1e"));
    palette.setColor(QPalette::AlternateBase, QColor("#262626"));
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::ToolTipBase, QColor("#3c3c3c"));
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::PlaceholderText, QColor("#8a8a8a"));
    palette.setColor(QPalette::Link, QColor("#5aa0ff"));
    palette.setColor(QPalette::Highlight, QColor("#0a64c8"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) palette.setColor(QPalette::Disabled, role, disabled);
    return palette;
}

// theme is "system", "light" or "dark". System follows the OS and its later changes.
inline QString& currentTheme() { static QString theme = "light"; return theme; }
// Explicit choices win even where the platform cannot switch its own color scheme.
inline bool themeIsDark() {
    const auto& theme = currentTheme();
    return theme == "dark" || (theme == "system" && QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
}
inline void applyTheme(QApplication& app, const QString& theme) {
    auto* hints = QGuiApplication::styleHints(); currentTheme() = theme;
    hints->setColorScheme(theme == "dark" ? Qt::ColorScheme::Dark : theme == "light" ? Qt::ColorScheme::Light : Qt::ColorScheme::Unknown);
    static bool following = false;
    if (!following) {
        following = true;
        QObject::connect(hints, &QStyleHints::colorSchemeChanged, &app, [&app] { app.setPalette(themePalette(themeIsDark())); });
    }
    app.setPalette(themePalette(themeIsDark()));
}

// The main window's stylesheet is written for light; dark swaps its colors.
inline QString themeStyleSheet(QString sheet, bool dark) {
    if (!dark) return sheet;
    const std::pair<const char*, const char*> swaps[] = {
        {"background: white", "background: #1e1e1e"}, {"#ffffff", "#1e1e1e"}, {"#f0f0f0", "#2b2b2b"}, {"#dedede", "#1a1a1a"},
        {"#b5b5b5", "#3f3f3f"}, {"#cce8ff", "#264f78"}, {"#202020", "#e6e6e6"}, {"#ababab", "#4a4a4a"}, {"#eeeeee", "#333333"},
        {"#dadada", "#333333"}, {"#505050", "#b0b0b0"}, {"#666666", "#9a9a9a"}};
    for (const auto& [light, replacement] : swaps) sheet.replace(QLatin1String(light), QLatin1String(replacement));
    return sheet;
}

inline void applyLightAppearance(QApplication& app) {
    app.setStyle(new AppStyle);
    app.setPalette(themePalette(false));
}
