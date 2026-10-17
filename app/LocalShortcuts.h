#pragma once
#include <QKeySequence>
inline QKeySequence preferencesShortcut() {
    const QKeySequence standard(QKeySequence::Preferences);
    return standard.isEmpty() ? QKeySequence(Qt::CTRL | Qt::Key_Comma) : standard;
}
