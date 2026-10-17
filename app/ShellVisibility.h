#pragma once
#include <QWidget>
#include <QVariant>
inline void setShellVisible(QWidget* widget, bool visible) {
    if (widget->property("presentationSuppressed").toBool()) widget->setProperty("requestedVisible", visible);
    else widget->setVisible(visible);
}
