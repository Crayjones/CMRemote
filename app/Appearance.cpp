#include "Appearance.h"
#include "DockManager.h"
#include "IconProvider.h"
#include <QFile>
#include <QIconEngine>
#include <QPainter>
#include <QProxyStyle>
#include <QStyleHints>

static void initThemeResources() { Q_INIT_RESOURCE(theme); }

namespace {
// Fusion, with a question-mark icon that stays visible on dark dialogs.
class AppStyle : public QProxyStyle {
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
        return QProxyStyle::standardIcon(icon, option, widget);
    }
};

// Crisp 10 px dock title-bar glyphs in the current palette's text color, drawn
// at paint time so they follow theme changes without being replaced.
class DockGlyph final : public QIconEngine {
public:
    explicit DockGlyph(ads::eIcon glyph) : glyph_(glyph) {}
    QIconEngine* clone() const override { return new DockGlyph(glyph_); }
    QString key() const override { return "cmremote-dock-glyph"; }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override {
        auto color = QApplication::palette().color(QPalette::ButtonText);
        if (mode == QIcon::Disabled) color.setAlphaF(0.35);
        painter->save(); painter->setRenderHint(QPainter::Antialiasing);
        const qreal side = qMin(rect.width(), rect.height()), scale = side / 10.0;
        painter->translate(rect.x() + (rect.width() - side) / 2, rect.y() + (rect.height() - side) / 2); painter->scale(scale, scale);
        painter->setPen(QPen(color, 1.2, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin)); painter->setBrush(Qt::NoBrush);
        switch (glyph_) {
        case ads::TabCloseIcon: case ads::DockAreaCloseIcon:
            painter->drawLine(QPointF(1.5, 1.5), QPointF(8.5, 8.5)); painter->drawLine(QPointF(8.5, 1.5), QPointF(1.5, 8.5)); break;
        case ads::DockAreaUndockIcon:
            painter->drawRect(QRectF(1.5, 3.5, 5, 5)); painter->drawPolyline(QPolygonF{{3.5, 3.5}, {3.5, 1.5}, {8.5, 1.5}, {8.5, 6.5}, {6.5, 6.5}}); break;
        case ads::DockAreaMenuIcon:
            painter->setPen(Qt::NoPen); painter->setBrush(color); painter->drawPolygon(QPolygonF{{1.5, 3.5}, {8.5, 3.5}, {5, 7.5}}); break;
        case ads::DockAreaMinimizeIcon:
            painter->drawLine(QPointF(1.5, 8.5), QPointF(8.5, 8.5)); break;
        case ads::AutoHideIcon: {
            // Visual Studio's push pin, standing: head, pin body and needle.
            painter->drawRect(QRectF(3, 1, 4, 4.5)); painter->drawLine(QPointF(5.5, 1.5), QPointF(5.5, 5));
            painter->drawLine(QPointF(1.5, 5.5), QPointF(8.5, 5.5)); painter->drawLine(QPointF(5, 5.5), QPointF(5, 9.5)); break;
        }
        default: break;
        }
        painter->restore();
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override { return scaledPixmap(size, mode, state, 1.0); }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        QPixmap pixmap((QSizeF(size) * scale).toSize()); pixmap.setDevicePixelRatio(scale); pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap); paint(&painter, QRect(QPoint(), size), mode, state); return pixmap;
    }
private:
    ads::eIcon glyph_;
};

QString styleSheet() {
    QFile file(":/theme/theme.qss");
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}
}

QPalette themePalette(bool dark) {
    QPalette palette;
    const auto set = [&palette](QPalette::ColorRole role, const QColor& color) { palette.setColor(role, color); };
    if (!dark) {
        // The classic Windows look.
        set(QPalette::Window, "#f0f0f0"); set(QPalette::WindowText, "#202020");
        set(QPalette::Base, Qt::white); set(QPalette::AlternateBase, "#f7f7f7"); set(QPalette::Text, "#202020");
        set(QPalette::Button, "#e5e5e5"); set(QPalette::ButtonText, "#202020");
        set(QPalette::Light, Qt::white); set(QPalette::Midlight, "#cce8ff"); set(QPalette::Mid, "#ababab");
        set(QPalette::Dark, "#a0a0a0"); set(QPalette::Shadow, "#696969"); set(QPalette::BrightText, Qt::white);
        set(QPalette::ToolTipBase, "#ffffe1"); set(QPalette::ToolTipText, "#202020");
        set(QPalette::PlaceholderText, "#6d6d6d"); set(QPalette::Link, "#0066cc");
        set(QPalette::Highlight, "#0078d7"); set(QPalette::HighlightedText, Qt::white); set(QPalette::Accent, "#0078d7");
        for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) palette.setColor(QPalette::Disabled, role, QColor("#808080"));
        return palette;
    }
    // Visual Studio Dark, as mRemoteNG's dark theme uses it.
    set(QPalette::Window, "#252526"); set(QPalette::WindowText, "#f1f1f1");
    set(QPalette::Base, "#1e1e1e"); set(QPalette::AlternateBase, "#252526"); set(QPalette::Text, "#f1f1f1");
    set(QPalette::Button, "#2d2d30"); set(QPalette::ButtonText, "#f1f1f1");
    set(QPalette::Light, "#d0d0d0"); set(QPalette::Midlight, "#3e3e40"); set(QPalette::Mid, "#3f3f46");
    set(QPalette::Dark, "#2d2d30"); set(QPalette::Shadow, "#121212"); set(QPalette::BrightText, Qt::white);
    set(QPalette::ToolTipBase, "#424245"); set(QPalette::ToolTipText, "#f1f1f1");
    set(QPalette::PlaceholderText, "#999999"); set(QPalette::Link, "#3794ff");
    set(QPalette::Highlight, "#0078d7"); set(QPalette::HighlightedText, Qt::white); set(QPalette::Accent, "#007acc");
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) palette.setColor(QPalette::Disabled, role, QColor("#6d6d6d"));
    palette.setColor(QPalette::Disabled, QPalette::Base, QColor("#252526"));
    return palette;
}

QString& currentTheme() { static QString theme = "light"; return theme; }
bool themeIsDark() {
    const auto& theme = currentTheme();
    return theme == "dark" || (theme == "system" && QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
}

static void applyPalette(QApplication& app) {
    app.setPalette(themePalette(themeIsDark()));
    // palette() references in the stylesheet resolve when it is applied.
    app.setStyleSheet(app.styleSheet());
}

void applyAppearance(QApplication& app) {
    initThemeResources();
    QApplication::setStyle(new AppStyle);
    for (const auto glyph : {ads::TabCloseIcon, ads::AutoHideIcon, ads::DockAreaMenuIcon, ads::DockAreaUndockIcon, ads::DockAreaCloseIcon, ads::DockAreaMinimizeIcon})
        ads::CDockManager::iconProvider().registerCustomIcon(glyph, QIcon(new DockGlyph(glyph)));
    app.setPalette(themePalette(themeIsDark()));
    app.setStyleSheet(styleSheet());
}

void applyTheme(QApplication& app, const QString& theme) {
    auto* hints = QGuiApplication::styleHints(); currentTheme() = theme;
    hints->setColorScheme(theme == "dark" ? Qt::ColorScheme::Dark : theme == "light" ? Qt::ColorScheme::Light : Qt::ColorScheme::Unknown);
    static bool following = false;
    if (!following) {
        following = true;
        QObject::connect(hints, &QStyleHints::colorSchemeChanged, &app, [&app] { applyPalette(app); });
    }
    applyPalette(app);
}
