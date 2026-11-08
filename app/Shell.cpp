#include "Shell.h"
#include "DockAreaWidget.h"
#include "DockManager.h"
#include "DockWidget.h"
#include "DockWidgetTab.h"
#include "DockAreaTitleBar.h"
#include "DockContainerWidget.h"
#include "FloatingDockContainer.h"
#include <QPointer>
#include <QSplitter>
#include <QStyle>
#include <algorithm>

void configureDocking() {
    using Manager = ads::CDockManager;
    // mRemoteNG's pane title bar: the pane name, a tab list when panes share an
    // area, pin and close. Tabs float by dragging them out of their area.
    Manager::setConfigFlags(Manager::DefaultOpaqueConfig);
    Manager::setConfigFlag(Manager::DisableStylesheet, true); // app/ui/theme.qss styles the docks
    Manager::setConfigFlag(Manager::FocusHighlighting, true); // the active pane's title bar turns blue
    Manager::setConfigFlag(Manager::DockAreaHasUndockButton, false);
    // Every tab may close; theme.qss shows the button only on session tabs.
    Manager::setConfigFlag(Manager::AllTabsHaveCloseButton, true);
    Manager::setConfigFlag(Manager::MiddleMouseButtonClosesTab, true);
    Manager::setConfigFlag(Manager::DockAreaCloseButtonClosesTab, true);
    Manager::setConfigFlag(Manager::DockAreaDynamicTabsMenuButtonVisibility, true);
    // Session panels cannot pin, so they show no disabled pin button.
    Manager::setConfigFlag(Manager::DockAreaHideDisabledButtons, true);
    Manager::setConfigFlag(Manager::FloatingContainerHasWidgetTitle, true);
    Manager::setConfigFlag(Manager::FloatingContainerHasWidgetIcon, true);
    Manager::setConfigFlag(Manager::DoubleClickUndocksWidget, false);
    Manager::setConfigFlag(Manager::XmlCompressionEnabled, false);
    Manager::setAutoHideConfigFlags(Manager::DefaultAutoHideConfig);
    Manager::setAutoHideConfigFlag(Manager::AutoHideButtonTogglesArea, false);
    Manager::setAutoHideConfigFlag(Manager::AutoHideHasMinimizeButton, false);
    Manager::setAutoHideConfigFlag(Manager::AutoHideHasCloseButton, true);
}

static ads::CDockWidget* paneDock(ads::CDockManager* manager, const char* name, const QString& title, QWidget* contents) {
    auto* widget = new ads::CDockWidget(manager, title); widget->setObjectName(name);
    widget->setWidget(contents, ads::CDockWidget::ForceNoScrollArea);
    widget->setMinimumSizeHintMode(ads::CDockWidget::MinimumSizeHintFromContentMinimumSize);
    return widget;
}

ads::CDockWidget* createSessionDock(ads::CDockManager* manager, QWidget* contents, const QString& name) {
    // Qt ADS keys docks by object name, so every session's must be unique.
    auto* dock = paneDock(manager, "", contents->windowTitle(), contents); dock->setObjectName(name);
    dock->setProperty("sessionDock", true);
    // Sessions dock, tab, split and float, but stay out of the pinned side bars.
    dock->setFeatures(ads::CDockWidget::DockWidgetClosable | ads::CDockWidget::DockWidgetMovable | ads::CDockWidget::DockWidgetFloatable |
                      ads::CDockWidget::DockWidgetFocusable | ads::CDockWidget::CustomCloseHandling);
    dock->tabWidget()->setProperty("session", true);
    return dock;
}
bool isSessionDock(const ads::CDockWidget* dock) { return dock && dock->property("sessionDock").toBool(); }

static bool isSessionPanel(const ads::CDockAreaWidget* area) {
    const auto docks = area->dockWidgets();
    return std::any_of(docks.begin(), docks.end(), [](ads::CDockWidget* dock) { return isSessionDock(dock) || dock->objectName() == "documentsDock"; });
}

// A splitter child that is, or holds, a session panel.
static bool holdsSessionPanel(QWidget* widget) {
    if (auto* area = qobject_cast<ads::CDockAreaWidget*>(widget)) return isSessionPanel(area);
    const auto nested = widget->findChildren<ads::CDockAreaWidget*>();
    return std::any_of(nested.begin(), nested.end(), isSessionPanel);
}

// A session panel squeezed to nothing (a window dropped on an outer edge gets
// no room from QSplitter) takes half of a neighbouring panel, never the side panes'.
static void giveCollapsedPanelsRoom(ads::CDockAreaWidget* area) {
    auto* splitter = qobject_cast<QSplitter*>(area->parentWidget()); if (!splitter || !area->isVisible()) return;
    auto sizes = splitter->sizes(); const int at = splitter->indexOf(area);
    if (at < 0 || sizes.value(at) >= 40) return;
    int donor = -1;
    for (int i = 0; i < splitter->count(); ++i) {
        auto* sibling = splitter->widget(i); if (i == at || !sibling->isVisible()) continue;
        if (holdsSessionPanel(sibling) && (donor < 0 || sizes.at(i) > sizes.at(donor))) donor = i;
    }
    if (donor < 0) return;
    const int total = sizes.at(at) + sizes.at(donor);
    sizes[at] = total / 2; sizes[donor] = total - total / 2; splitter->setSizes(sizes);
}

void markSessionPanels(ads::CDockManager* manager) {
    QList<ads::CDockContainerWidget*> containers{manager};
    for (auto* floating : manager->floatingWidgets()) containers.append(floating->dockContainer());
    QList<ads::CDockAreaWidget*> panels;
    for (auto* container : containers) for (int i = 0; i < container->dockAreaCount(); ++i) {
        auto* area = container->dockArea(i); const bool panel = isSessionPanel(area);
        if (panel) panels << area;
        // Session panels take the space panels and window resizes free or claim; the side panes keep their size.
        auto policy = area->sizePolicy(); policy.setHorizontalStretch(panel ? 1 : 0); policy.setVerticalStretch(panel ? 1 : 0); area->setSizePolicy(policy);
        // Nothing docks inside the side panes' column, so sessions never end up squeezed among them;
        // panes and sessions still dock at the window's edges and into session panels.
        area->setAllowedAreas(panel ? ads::AllDockAreas : ads::NoDockWidgetArea);
        if (area->property("sessionPanel").toBool() == panel) continue;
        area->setProperty("sessionPanel", panel);
        for (auto* widget : QList<QWidget*>{area, area->titleBar()}) { widget->style()->unpolish(widget); widget->style()->polish(widget); }
    }
    for (auto* panel : panels) giveCollapsedPanelsRoom(panel);
}

void showGeneralPlaceholder(ads::CDockWidget* placeholder, bool show) {
    if (placeholder->isClosed() != show) return;
    auto* area = placeholder->dockAreaWidget();
    // The sizes of every splitter around General, from its own outwards, before it hides.
    QList<std::pair<QPointer<QSplitter>, QList<int>>> chain;
    for (QWidget* widget = area; widget; widget = widget->parentWidget())
        if (auto* splitter = qobject_cast<QSplitter*>(widget->parentWidget())) chain.append({splitter, splitter->sizes()}); else break;
    placeholder->toggleView(show);
    if (show || chain.isEmpty() || area->isVisible()) return;
    // An emptied General's room goes to the nearest session panels, not to the side panes:
    // every splitter up to theirs shrinks General's branch by what General had.
    // (Its splitter handle goes with it.)
    const int freed = chain.first().second.value(chain.first().first->indexOf(area)) + chain.first().first->handleWidth();
    QWidget* branch = area;
    for (int level = 0; level < chain.size(); ++level) {
        auto& [splitter, sizes] = chain[level]; if (!splitter) return;
        const int at = splitter->indexOf(branch); if (at < 0) return;
        sizes[at] = level == 0 ? 0 : std::max(0, sizes.at(at) - freed);
        QList<int> panels;
        for (int i = 0; i < splitter->count(); ++i) if (i != at && splitter->widget(i)->isVisible() && holdsSessionPanel(splitter->widget(i))) panels << i;
        if (!panels.isEmpty()) {
            for (int i = 0; i < panels.size(); ++i) sizes[panels.at(i)] += freed / int(panels.size()) + (i == 0 ? freed % int(panels.size()) : 0);
            for (int outer = level; outer >= 0; --outer) if (chain.at(outer).first) chain.at(outer).first->setSizes(chain.at(outer).second);
            return;
        }
        branch = splitter;
    }
}

void splitPanel(ads::CDockManager* manager, ads::CDockWidget* dock, ads::CDockAreaWidget* panel, int side) {
    const bool across = side == ads::LeftDockWidgetArea || side == ads::RightDockWidgetArea;
    const int span = across ? panel->width() : panel->height();
    manager->addDockWidget(ads::DockWidgetArea(side), dock, panel);
    // The new panel takes half of the one it split from; other panes keep their size.
    if (auto* splitter = qobject_cast<QSplitter*>(panel->parentWidget()); splitter && splitter == dock->dockAreaWidget()->parentWidget()) {
        auto sizes = splitter->sizes(); const int half = (span - splitter->handleWidth()) / 2;
        sizes[splitter->indexOf(panel)] = half; sizes[splitter->indexOf(dock->dockAreaWidget())] = span - splitter->handleWidth() - half;
        splitter->setSizes(sizes);
    }
}

void ShellUi::setupUi(QMainWindow* window) {
    Ui_MainWindow::setupUi(window);
    auto* connections = new QWidget; Ui_ConnectionsPane::setupUi(connections);
    auto* config = new QWidget; Ui_ConfigPane::setupUi(config);
    auto* notifications = new QWidget; Ui_NotificationsPane::setupUi(notifications);
    // The pane toolbars share the window's actions with its menus.
    connectionsToolbar->addActions({actionAddConnection, actionAddFolder, actionExpandAll, actionSort});
    configToolbar->addActions({actionCategorized, actionAlphabetical}); configToolbar->addSeparator();
    configToolbar->addActions({actionProperties, actionInheritance, actionDefaultProperties, actionDefaultInheritance}); configToolbar->addSeparator();
    configToolbar->addActions({actionConnectionIcon, actionHostStatus});

    configureDocking();
    // The dock manager becomes the window's central widget.
    auto* empty = window->takeCentralWidget();
    dockManager = new ads::CDockManager(window); dockManager->setObjectName("dockManager");
    // The General panel holds this tabless placeholder, so it keeps its place
    // in the layout while every session is closed or moved elsewhere.
    documentsDock = paneDock(dockManager, "documentsDock", "General", empty);
    documentsDock->setFeatures(ads::CDockWidget::NoTab);
    dockManager->addDockWidget(ads::CenterDockWidgetArea, documentsDock);
    connectionsDock = paneDock(dockManager, "connectionsDock", "Connections", connections);
    configDock = paneDock(dockManager, "configDock", "Config", config);
    notificationsDock = paneDock(dockManager, "notificationsDock", "Notifications", notifications);
    resetDocks();
}

ads::CDockAreaWidget* ShellUi::generalPanel() const { return documentsDock->dockAreaWidget(); }

static void undock(ads::CDockWidget* dock) {
    // Open before moving: a closed pane re-added to a new area stays hidden.
    dock->toggleView(true);
    if (dock->isAutoHide()) dock->setAutoHide(false);
    if (auto* manager = dock->dockManager()) manager->removeDockWidget(dock);
}

void ShellUi::resetDocks() {
    undock(connectionsDock); undock(configDock); undock(notificationsDock);
    auto* left = dockManager->addDockWidget(ads::LeftDockWidgetArea, connectionsDock);
    dockManager->addDockWidget(ads::BottomDockWidgetArea, configDock, left);
    // Fixed starting sizes like mRemoteNG's: a 226 px column, Config a little taller.
    const int width = qMax(dockManager->width(), 640), height = qMax(dockManager->height(), 480);
    dockManager->setSplitterSizes(left, {height * 21 / 46, height * 25 / 46});
    auto* bottom = dockManager->addDockWidget(ads::BottomDockWidgetArea, notificationsDock);
    dockManager->setSplitterSizes(generalPanel(), {226, width - 230});
    // A short strip along the bottom, kept for when the pane is opened.
    dockManager->setSplitterSizes(bottom, {height - 95, 95});
    notificationsDock->toggleView(false);
    // Connections starts as the active pane, as in mRemoteNG.
    dockManager->setDockWidgetFocused(connectionsDock);
}
