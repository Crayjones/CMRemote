#pragma once
#include <QApplication>
#include <QDockWidget>
#include <QPainter>
#include <QPalette>
#include <QProxyStyle>

// Fusion with white float/close glyphs on the blue dock title bars.
class AppStyle : public QProxyStyle {
public:
    AppStyle() : QProxyStyle("Fusion") {}
    QIcon standardIcon(StandardPixmap icon, const QStyleOption* option, const QWidget* widget) const override {
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

inline void applyLightAppearance(QApplication& app) {
    app.setStyle(new AppStyle);
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
    app.setPalette(palette);
}
