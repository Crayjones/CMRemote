#pragma once
#include "SessionPane.h"
#include "DockAreaWidget.h"
#include "DockAreaTitleBar.h"
#include "DockAreaTabBar.h"
#include "DockWidget.h"
#include "DockWidgetTab.h"
#include "FloatingDockContainer.h"
#include "FloatingDragPreview.h"
#include <QMenu>
#include <QApplication>
#include <QContextMenuEvent>
#include <QPointer>

// A test's view of a panel: the Qt ADS dock area holding session tabs. Reads
// like the QTabWidget the panels used to be, so tests index tabs by position.
inline bool isSessionDock(const ads::CDockWidget* dock) { return dock && dock->property("sessionDock").toBool(); }

class SessionTabs {
public:
    explicit SessionTabs(ads::CDockAreaWidget* area = nullptr) : area_(area) {}
    ads::CDockAreaWidget* area() const { return area_; }
    explicit operator bool() const { return area_; }
    const SessionTabs* operator->() const { return this; }
    QList<ads::CDockWidget*> docks() const {
        QList<ads::CDockWidget*> result;
        if (area_) for (auto* dock : area_->dockWidgets()) if (isSessionDock(dock)) result.append(dock);
        return result;
    }
    int count() const { return int(docks().size()); }
    SessionPane* widget(int index) const {
        const auto all = docks(); return index >= 0 && index < all.size() ? qobject_cast<SessionPane*>(all.at(index)->widget()) : nullptr;
    }
    ads::CDockWidget* dock(int index) const { const auto all = docks(); return index >= 0 && index < all.size() ? all.at(index) : nullptr; }
    ads::CDockWidgetTab* tab(int index) const { auto* found = dock(index); return found ? found->tabWidget() : nullptr; }
    int indexOf(const QWidget* pane) const {
        const auto all = docks();
        for (int i = 0; i < all.size(); ++i) if (all.at(i)->widget() == pane) return i;
        return -1;
    }
    SessionPane* currentWidget() const { return area_ ? qobject_cast<SessionPane*>(area_->currentDockWidget() ? area_->currentDockWidget()->widget() : nullptr) : nullptr; }
    int currentIndex() const { return indexOf(currentWidget()); }
    void setCurrentIndex(int index) const { if (auto* found = dock(index)) found->setAsCurrentTab(); }
    void setCurrentWidget(QWidget* pane) const { setCurrentIndex(indexOf(pane)); }
    QString tabText(int index) const { auto* found = dock(index); return found ? found->windowTitle() : QString(); }
    QIcon tabIcon(int index) const { auto* found = dock(index); return found ? found->icon() : QIcon(); }
    bool tabBarVisible() const { return area_ && area_->titleBar()->isVisible(); }
private:
    QPointer<ads::CDockAreaWidget> area_;
};

inline ads::CDockWidget* sessionDockOf(const QWidget* pane) {
    for (auto* widget = pane ? pane->parentWidget() : nullptr; widget; widget = widget->parentWidget())
        if (auto* dock = qobject_cast<ads::CDockWidget*>(widget)) return dock;
    return nullptr;
}
// The General panel, where sessions open unless another panel was used.
inline SessionTabs sessionTabs(const QWidget& window) {
    auto* placeholder = window.findChild<ads::CDockWidget*>("documentsDock");
    return SessionTabs(placeholder ? placeholder->dockAreaWidget() : nullptr);
}
// The panel a session's tab is in, wherever that is.
inline SessionTabs panelTabs(const QWidget* pane) {
    auto* dock = sessionDockOf(pane); return SessionTabs(dock ? dock->dockAreaWidget() : nullptr);
}
// Right-clicks a session tab, as the user opens its context menu.
inline void openTabMenu(const SessionTabs& tabs, int index) {
    auto* tab = tabs.tab(index); if (!tab) return;
    const QPoint point = tab->rect().center();
    QContextMenuEvent event(QContextMenuEvent::Mouse, point, tab->mapToGlobal(point)); QApplication::sendEvent(tab, &event);
}
// Visible floating windows: tabs dragged or moved out of every panel.
inline QList<ads::CFloatingDockContainer*> floatingWindows() {
    QList<ads::CFloatingDockContainer*> result;
    for (auto* widget : QApplication::topLevelWidgets())
        if (auto* window = qobject_cast<ads::CFloatingDockContainer*>(widget); window && window->isVisible()) result << window;
    return result;
}
// Runs a command from a session tab's context menu.
inline bool tabMenuCommand(QWidget& window, const SessionTabs& tabs, int index, const char* name) {
    openTabMenu(tabs, index);
    const auto menus = window.findChildren<QMenu*>("sessionContextMenu"); if (menus.isEmpty()) return false;
    auto* action = menus.last()->findChild<QAction*>(name); if (!action || !action->isEnabled()) { menus.last()->close(); return false; }
    menus.last()->close(); action->trigger(); return true;
}
