#pragma once
#include "PropertyDelegate.h"
#include "ProfileEditor.h"
#include "XmlTestHelpers.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QTest>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#ifdef Q_OS_MACOS
void activateNativeTestApplication(QWidget* widget);
void toggleNativeFullScreen(QWidget* widget);
bool isNativeFullScreen(QWidget* widget);
bool isNativeVisible(QWidget* widget);
bool isNativeMiniaturized(QWidget* widget);
void deminiaturizeNative(QWidget* widget);
// False on macOS until Screen Recording access was granted; never prompts.
bool screenCaptureAllowed();
#else
inline bool screenCaptureAllowed() { return true; }
#endif

inline QTreeWidgetItem* gridRow(QWidget& window, const QString& key) {
    auto* tree = window.findChild<QTreeWidget*>("configTree");
    for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->data(0, PropertyDelegate::KeyRole).toString() == key) return *it;
    qFatal("Missing Config property row");
}
inline QVariant gridValue(QWidget& window, const QString& key) { return gridRow(window, key)->data(1, PropertyDelegate::ValueRole); }
inline bool gridLocal(QWidget& window, const QString& key) {
    const auto overrides = window.findChild<ProfileEditor*>()->draft().value("overrides").toObject();
    return overrides.contains(key) && !overrides.value(key).isNull();
}
inline bool waitGridSaved(QWidget& window) {
    return QTest::qWaitFor([&] { return !documentBusy(static_cast<MainWindow&>(window)) && !window.findChild<ProfileEditor*>()->dirty(); });
}
inline void closeGridEditors(QWidget& window) {
    auto* tree = window.findChild<QTreeWidget*>("configTree");
    auto* delegate = qobject_cast<PropertyDelegate*>(tree->itemDelegate());
    if (QWidget* open = delegate->activeEditor()) { open->setFocus(); QTest::keyClick(open, Qt::Key_Escape); }
    if (!QTest::qWaitFor([&] { return delegate->activeEditor() == nullptr && tree->findChild<QLineEdit*>() == nullptr &&
        tree->findChild<QComboBox*>() == nullptr && tree->findChild<QSpinBox*>() == nullptr; })) qFatal("Config editor did not close");
}
inline QWidget* gridEditor(QWidget& window, const QString& key, bool mouse = true) {
    if (!window.isVisible()) window.show();
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() == "cocoa") {
        activateNativeTestApplication(&window); window.raise(); window.activateWindow();
        if (!QTest::qWaitForWindowActive(&window)) qFatal("Config window did not activate");
    }
#endif
    auto* tree = window.findChild<QTreeWidget*>("configTree");
    closeGridEditors(window);
    auto* row = gridRow(window, key);
    if (row->parent()) row->parent()->setExpanded(true);
    tree->scrollToItem(row); tree->setCurrentItem(row, 1); tree->setFocus();
    auto* delegate = qobject_cast<PropertyDelegate*>(tree->itemDelegate());
    if (mouse) {
        const auto cell = tree->visualItemRect(row);
        const QPoint point(tree->columnViewportPosition(1) + tree->columnWidth(1) / 2, cell.center().y());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        if (!delegate->activeEditor()) {
            QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, point);
            QTest::mouseRelease(tree->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        }
    } else QTest::keyClick(tree, Qt::Key_Return);
    if (!QTest::qWaitFor([&] { return delegate->activeEditor() != nullptr; })) qFatal("Config %s editor did not open via %s", qPrintable(key), mouse ? "mouse" : "Enter");
    return delegate->activeEditor();
}
inline void setGridText(QWidget& window, const QString& key, const QString& text) {
    auto* edit = qobject_cast<QLineEdit*>(gridEditor(window, key)); if (!edit) qFatal("Expected Config text editor");
    edit->selectAll(); QTest::keyClick(edit, Qt::Key_Backspace); QTest::keyClicks(edit, text); QTest::keyClick(edit, Qt::Key_Return);
}
inline void setGridChoice(QWidget& window, const QString& key, const QVariant& value) {
    auto* combo = qobject_cast<QComboBox*>(gridEditor(window, key)); if (!combo) qFatal("Expected Config choice editor");
    const auto index = combo->findData(value); if (index < 0) qFatal("Missing Config choice");
    QTest::keyClick(combo, Qt::Key_Home);
    for (int i = 0; i < index; ++i) QTest::keyClick(combo, Qt::Key_Down);
    QTest::keyClick(combo, Qt::Key_Return);
}
inline void setGridPort(QWidget& window, int port) {
    auto* spin = qobject_cast<QSpinBox*>(gridEditor(window, "port")); if (!spin) qFatal("Expected Config port editor");
    auto* edit = spin->findChild<QLineEdit*>(); edit->selectAll(); QTest::keyClicks(edit, QString::number(port)); QTest::keyClick(spin, Qt::Key_Return);
}
inline void setGridLocal(QWidget& window, const QString& key, bool local) {
    if (gridLocal(window, key) == local) return;
    window.findChild<QAction*>("actionInheritance")->trigger();
    auto* combo = qobject_cast<QComboBox*>(gridEditor(window, key)); if (!combo) qFatal("Expected Config inheritance editor");
    QTest::keyClick(combo, local ? Qt::Key_Down : Qt::Key_Up); QTest::keyClick(combo, Qt::Key_Return);
    if (!waitGridSaved(window)) qFatal("Config inheritance edit did not save");
    window.findChild<QAction*>("actionProperties")->trigger();
    if (gridLocal(window, key) != local) qFatal("Config inheritance control did not change");
}
