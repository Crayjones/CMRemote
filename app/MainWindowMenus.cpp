#include "MainWindow.h"
#include "ThemeIcons.h"
#include "ProfileEditor.h"
#include "DockManager.h"
#include "DockWidget.h"
#include "SessionPane.h"
#include "Views.h"
#include "FullscreenWindow.h"
#include "DockAreaWidget.h"
#include "DockAreaTitleBar.h"
#include "DockContainerWidget.h"
#include "DockWidgetTab.h"
#include "FloatingDockContainer.h"
#include "SessionFullscreenController.h"
#include <QMouseEvent>
#include <QSignalBlocker>
#include "Shell.h"
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QInputDialog>
#include <QFileDialog>
#include <QJsonArray>
#include <QMenu>
#include <QMessageBox>
#include <QPersistentModelIndex>
#include <QTimer>
#include <QStyle>
#include <algorithm>
#ifdef Q_OS_MACOS
void rmNativeKeepHiddenAfterFullScreen(QWidget* widget);
#endif

ads::CDockWidget* MainWindow::dockOf(const SessionPane* pane) {
    for (auto* widget = pane ? pane->parentWidget() : nullptr; widget; widget = widget->parentWidget())
        if (auto* dock = qobject_cast<ads::CDockWidget*>(widget)) return dock;
    return nullptr;
}
SessionPane* MainWindow::paneOf(const ads::CDockWidget* dock) {
    // Not dock->widget(): Qt ADS keeps a raw pointer to a session destroyed outside the shell.
    return isSessionDock(dock) ? dock->findChild<SessionPane*>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
}
QList<ads::CDockAreaWidget*> MainWindow::sessionPanels() const {
    QList<ads::CDockAreaWidget*> result;
    auto collect = [&](ads::CDockContainerWidget* container) {
        for (int i = 0; i < container->dockAreaCount(); ++i) {
            auto* area = container->dockArea(i); const auto docks = area ? area->dockWidgets() : QList<ads::CDockWidget*>{};
            if (std::any_of(docks.begin(), docks.end(), [](ads::CDockWidget* dock) { return isSessionDock(dock); })) result.append(area);
        }
    };
    // General first, then the main window's other panels, then floating windows'.
    collect(ui_->dockManager);
    if (result.removeAll(ui_->generalPanel())) result.prepend(ui_->generalPanel());
    for (auto* floating : ui_->dockManager->floatingWidgets()) collect(floating->dockContainer());
    return result;
}
QList<SessionPane*> MainWindow::panelPanes(ads::CDockAreaWidget* panel) const {
    QList<SessionPane*> result;
    if (panel) for (auto* dock : panel->dockWidgets()) if (auto* pane = paneOf(dock)) result.append(pane);
    return result;
}
QList<ads::CDockWidget*> MainWindow::sessionDocks() const {
    QList<ads::CDockWidget*> result;
    for (auto* panel : sessionPanels()) for (auto* dock : panel->dockWidgets()) if (isSessionDock(dock)) result.append(dock);
    for (const auto& entry : fullscreenWindows_) if (entry.window) if (auto* dock = entry.window->sessionDock()) result.append(dock);
    return result;
}
QList<SessionPane*> MainWindow::sessionPages() const {
    QList<SessionPane*> result;
    for (auto* dock : sessionDocks()) if (auto* pane = paneOf(dock)) result.append(pane);
    return result;
}
bool MainWindow::isOpenPane(const SessionPane* pane) const {
    auto* dock = dockOf(pane); return dock && sessionDocks().contains(dock);
}
ads::CDockAreaWidget* MainWindow::panelOf(const SessionPane* pane) const {
    auto* dock = dockOf(pane); return dock && isOpenPane(pane) ? dock->dockAreaWidget() : nullptr;
}
QString MainWindow::panelName(ads::CDockAreaWidget* panel) const {
    if (!panel) return {};
    if (panel == ui_->generalPanel()) return "General";
    // Panels are numbered when first named and keep their number.
    if (panel->property("panelName").toString().isEmpty()) panel->setProperty("panelName", QString("Panel %1").arg(nextPanel_++));
    const auto name = panel->property("panelName").toString();
    return panel->dockContainer() && panel->dockContainer()->isFloating() ? name + " (window)" : name;
}
ads::CDockAreaWidget* MainWindow::currentPanel() const {
    // New sessions open in the panel whose session was used last, unless that
    // session is fullscreen in its own window.
    if (lastSession_ && lastSession_->dockManager() == ui_->dockManager)
        if (auto* area = lastSession_->dockAreaWidget(); area && sessionPanels().contains(area)) return area;
    for (auto* panel : sessionPanels()) if (!panel->dockContainer()->isFloating()) return panel;
    return ui_->generalPanel();
}
SessionPane* MainWindow::currentPane() const {
    auto* panel = currentPanel(); return panel ? paneOf(panel->currentDockWidget()) : nullptr;
}
FullscreenWindow* MainWindow::fullscreenWindowOf(const SessionPane* pane) const {
    for (const auto& entry : fullscreenWindows_) if (entry.window && pane && entry.window->isAncestorOf(pane)) return entry.window;
    return nullptr;
}
void MainWindow::exitFullscreenFor(SessionPane* pane) {
    // Exiting returns the session and retires its window, so walk a copy.
    const auto windows = fullscreenWindows_;
    for (const auto& entry : windows) if (entry.window && entry.window->fullscreen()->target() == pane) entry.window->fullscreen()->exit();
}
bool MainWindow::isFullscreenWindow(const QWidget* window) const {
    return std::any_of(fullscreenWindows_.begin(), fullscreenWindows_.end(), [window](const FullscreenEntry& entry) { return window && entry.window == window; });
}
QMainWindow* MainWindow::sessionFullscreenWindow() const {
    for (const auto& entry : fullscreenWindows_) if (entry.window && entry.window->fullscreen()->active()) return entry.window;
    return nullptr;
}
void MainWindow::focusPane(SessionPane* pane) {
    auto* dock = dockOf(pane); if (!dock || !isOpenPane(pane)) return;
    dock->setAsCurrentTab(); if (dock->dockManager() == ui_->dockManager) lastSession_ = dock;
    if (auto* floating = dock->dockContainer() ? dock->dockContainer()->floatingWidget() : nullptr) { floating->raise(); floating->activateWindow(); }
    updateActions();
}
void MainWindow::focusPanel(ads::CDockAreaWidget* panel) {
    if (!panel) return;
    if (auto* pane = paneOf(panel->currentDockWidget())) { focusPane(pane); pane->setFocus(); return; }
    auto* window = panel->window(); window->raise(); window->activateWindow();
}
void MainWindow::configureSessionDock(ads::CDockWidget* dock) {
    // Its tab offers the session's commands instead of Qt ADS's own menu.
    dock->tabWidget()->installEventFilter(this);
    // A session destroyed outside the shell takes its tab with it.
    connect(dock->widget(), &QObject::destroyed, this, [dock = QPointer<ads::CDockWidget>(dock)] {
        if (!dock) return;
        if (auto* manager = dock->dockManager()) manager->removeDockWidget(dock);
        dock->deleteLater();
    }, Qt::QueuedConnection);
    // After Qt ADS finishes handling the click that asked to close it.
    connect(dock, &ads::CDockWidget::closeRequested, this, [this, dock = QPointer<ads::CDockWidget>(dock)] {
        if (dock && !closeApproved_) closePane(paneOf(dock));
    }, Qt::QueuedConnection);
}
void MainWindow::sessionLayoutChanged() {
    if (sessionLayoutQueued_) return;
    sessionLayoutQueued_ = true;
    QTimer::singleShot(0, this, [this] { sessionLayoutQueued_ = false; updateSessionLayout(); });
}
void MainWindow::updateSessionLayout() {
    if (!ui_ || !ui_->dockManager) return;
    // General keeps its place in the layout through a tabless placeholder,
    // shown with no title bar only while the main window has no session.
    bool sessions = false;
    for (auto* panel : sessionPanels()) sessions = sessions || !panel->dockContainer()->isFloating();
    // theme.qss styles panels of session tabs like Visual Studio's documents. A panel
    // squeezed to nothing takes room from General before General hides and frees its own.
    markSessionPanels(ui_->dockManager);
    auto* general = ui_->generalPanel();
    if (general) general->setDockAreaFlag(ads::CDockAreaWidget::HideSingleWidgetTitleBar, !sessions);
    showGeneralPlaceholder(ui_->documentsDock, !sessions);
    if (lastSession_ && !isSessionDock(lastSession_)) lastSession_.clear();
    updateActions();
}
void MainWindow::closePane(SessionPane* pane) {
    auto* dock = dockOf(pane); if (!pane || !dock || !isOpenPane(pane)) return;
    // A fullscreen session closes in its own window instead of returning first.
    pane->setProperty("closingPane", true);
    disconnectPane(pane);
    sessionCore(pane->document()).close_session(pane->id().toUtf8().toStdString());
    QPointer<ads::CDockAreaWidget> panel = dock->dockAreaWidget();
    if (auto* manager = dock->dockManager()) manager->removeDockWidget(dock);
    delete dock;
    // The panel's next tab becomes the one in use.
    if (!lastSession_ && panel) if (auto* next = panel->currentDockWidget(); isSessionDock(next)) lastSession_ = next;
    updateSessionLayout();
}
void MainWindow::closeAllSessions() {
    if (pollTimer_) pollTimer_->stop();
    cancelCredentials();
    for (auto* pane : sessionPages()) closePane(pane);
    // Every session is gone, so no view still references a window's GPU context.
    fullscreenWindows_.clear();
    for (const auto& window : ownedFullscreen_) if (window) { window->hide(); window->deleteLater(); }
    ownedFullscreen_.clear();
    for (const auto& window : retiredWindows_) if (window) { window->hide(); window->deleteLater(); }
    retiredWindows_.clear();
}
void MainWindow::movePaneTo(SessionPane* pane, ads::CDockAreaWidget* destination, int index) {
    auto* dock = dockOf(pane); if (!dock || !destination || !isOpenPane(pane)) return;
    exitFullscreenFor(pane);
    if (credentialPage_ == pane) cancelCredentials();
    auto* manager = destination->dockManager(); if (!manager) return;
    if (dock->dockAreaWidget() == destination) {
        // Within a panel only the tab order changes.
        const int from = destination->dockWidgets().indexOf(dock), to = index < 0 ? destination->dockWidgetsCount() - 1 : qMin(index, destination->dockWidgetsCount() - 1);
        if (from != to) { manager->addDockWidgetTabToArea(dock, destination, to); }
    } else {
        if (dock->dockManager() && dock->dockManager() != manager) dock->dockManager()->removeDockWidget(dock);
        manager->addDockWidgetTabToArea(dock, destination, index);
    }
    dock->setAsCurrentTab(); updateSessionLayout();
}
void MainWindow::splitPane(SessionPane* pane, int side) {
    auto* dock = dockOf(pane); auto* panel = panelOf(pane);
    if (!dock || !panel || panelPanes(panel).size() < 2 || dock->dockManager() != ui_->dockManager) return;
    splitPanel(ui_->dockManager, dock, panel, side);
    focusPane(pane); updateSessionLayout();
}
void MainWindow::floatPane(SessionPane* pane) {
    auto* dock = dockOf(pane); if (!dock || !isOpenPane(pane) || dock->dockManager() != ui_->dockManager) return;
    exitFullscreenFor(pane);
    const auto size = dock->size();
    dock->setFloating();
    if (auto* floating = dock->dockContainer() ? dock->dockContainer()->floatingWidget() : nullptr) {
        // Beside the window it left rather than under the pointer.
        floating->resize(size.expandedTo(QSize(640, 480)));
        floating->move(window()->geometry().center() - QPoint(floating->width() / 2, floating->height() / 2) + QPoint(40, 40));
        floating->raise(); floating->activateWindow();
    }
    focusPane(pane); updateSessionLayout();
}
void MainWindow::addPanel() {
    if (closeApproved_) return;
    splitPane(currentPane(), ads::RightDockWidgetArea);
}
void MainWindow::requestCloseFloating(ads::CFloatingDockContainer* window) {
    if (!window) return;
    QList<QPointer<SessionPane>> panes; bool active = false;
    for (auto* dock : window->dockWidgets()) if (auto* pane = paneOf(dock)) { panes.append(pane); active = active || pane->active(); }
    QPointer<ads::CFloatingDockContainer> target = window;
    auto close = [this, target, panes] {
        if (!target) return;
        for (const auto& pane : panes) if (pane) closePane(pane);
        // Panes dragged into the window close as they would anywhere.
        if (target) for (auto* dock : target->dockWidgets()) if (!isSessionDock(dock)) dock->toggleView(false);
    };
    if (testMode_ || !active) { close(); return; }
    auto* dialog = new QMessageBox(QMessageBox::Question, "Close window", QString("Close this window and end its %1 session(s)?").arg(panes.size()),
                                  QMessageBox::Yes | QMessageBox::Cancel, window);
    dialog->setObjectName("closeWindow"); dialog->setTextFormat(Qt::PlainText); dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel); dialog->setWindowModality(Qt::WindowModal); dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [close](int answer) { if (answer == QMessageBox::Yes) close(); }); dialog->open();
}
void MainWindow::retireWindow(QWidget* window) {
    if (!window || retiredWindows_.contains(window)) return;
    window->hide();
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() == "cocoa") rmNativeKeepHiddenAfterFullScreen(window);
#endif
    // QRhiWidget keeps the GPU context of the last window it rendered in until
    // it renders again and dereferences it on destruction (Qt clears it only
    // when the view next draws). Keep this window and its context alive until
    // every session that existed meanwhile, and so might have drawn here, is gone.
    const auto panes = sessionPages();
    if (panes.isEmpty()) { window->deleteLater(); return; }
    retiredWindows_.append(window);
    auto remaining = std::make_shared<int>(int(panes.size()));
    for (auto* pane : panes) connect(pane, &QObject::destroyed, window, [this, window, remaining] {
        if (--*remaining == 0) { retiredWindows_.removeAll(window); window->deleteLater(); }
    });
}
void MainWindow::renamePane(SessionPane* pane) {
    auto* dock = dockOf(pane); if (!dock || !isOpenPane(pane)) return;
    QPointer<SessionPane> target = pane;
    auto* dialog = new QInputDialog(this); dialog->setObjectName("renamePage"); dialog->setWindowTitle("Rename tab");
    dialog->setLabelText("Name:"); dialog->setTextValue(dock->windowTitle());
    dialog->setWindowModality(Qt::WindowModal); dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QInputDialog::textValueSelected, this, [this, target](const QString& value) {
        if (!target || closeApproved_ || value.trimmed().isEmpty() || !isOpenPane(target)) return;
        dockOf(target)->setWindowTitle(value.trimmed());
    }); dialog->open();
}
void MainWindow::fullscreenPane(QPointer<SessionPane> pane) {
    // The session's dock moves to its own fullscreen window, leaving the window
    // it came from usable; exiting fullscreen returns it to the same tab position.
    guard([this, pane] {
        if (!pane || !isOpenPane(pane)) return;
        if (auto* current = fullscreenWindowOf(pane); current && current->fullscreen()->active()) { current->fullscreen()->exit(); return; }
        exitFullscreenFor(pane);
        auto* dock = dockOf(pane); auto* panel = dock->dockAreaWidget(); auto* host = dock->window();
        auto* window = new FullscreenWindow; window->setWindowTitle(dock->windowTitle()); ownedFullscreen_.append(window);
        fullscreenWindows_.append({window, panel, panel ? int(panel->dockWidgets().indexOf(dock)) : -1});
        connect(window->fullscreen(), &SessionFullscreenController::closeRequested, this, &MainWindow::closePane);
        connect(window, &FullscreenWindow::closeRequested, this, [this](FullscreenWindow* window) {
            if (auto* pane = paneOf(window->sessionDock())) closePane(pane); else retireWindow(window);
        });
        connect(window->fullscreen(), &SessionFullscreenController::exited, this,
                [this, target = QPointer<FullscreenWindow>(window)](SessionPane* left) { returnFromFullscreen(target, left); });
        if (dock->dockManager()) dock->dockManager()->removeDockWidget(dock);
        window->dockManager()->addDockWidget(ads::CenterDockWidgetArea, dock);
        if (lastSession_ == dock) lastSession_.clear();
        updateSessionLayout();
        window->setGeometry(host->geometry()); window->show(); window->fullscreen()->enter(pane, dock->windowTitle());
        updateActions();
    });
}
void MainWindow::returnFromFullscreen(QPointer<FullscreenWindow> window, QPointer<SessionPane> pane) {
    const auto entry = std::find_if(fullscreenWindows_.begin(), fullscreenWindows_.end(), [&](const FullscreenEntry& item) { return item.window == window; });
    if (entry == fullscreenWindows_.end()) return;
    const auto origin = entry->origin; const int index = entry->index; fullscreenWindows_.erase(entry);
    const bool closing = !pane || pane->property("closingPane").toBool();
    if (!closing && window && window->isAncestorOf(pane)) {
        // The panel it left may have closed meanwhile; General always exists.
        auto* dock = dockOf(pane); window->dockManager()->removeDockWidget(dock);
        auto* destination = origin && origin->dockManager() == ui_->dockManager ? origin.data() : currentPanel();
        ui_->dockManager->addDockWidgetTabToArea(dock, destination, index);
        updateSessionLayout(); focusPane(pane);
        auto* host = dock->window(); host->raise(); host->activateWindow();
    }
    // A closing or destroyed session is still leaving the window; retire it after.
    auto retire = [this, window] { if (window) retireWindow(window); };
    if (closing) QTimer::singleShot(0, this, retire); else retire();
}
void MainWindow::resetLayout() {
    ui_->resetDocks();
    for (auto* toolbar : {ui_->quickConnectToolbar, ui_->externalToolsToolbar, ui_->multiSshToolbar}) {
        removeToolBar(toolbar); addToolBar(Qt::TopToolBarArea, toolbar);
        toolbar->setMovable(!ui_->actionLockToolbars->isChecked() && !(quickConnectInTitleBar_ && toolbar == ui_->quickConnectToolbar));
    }
    ui_->quickConnectToolbar->show(); ui_->externalToolsToolbar->hide(); ui_->multiSshToolbar->hide();
}
void MainWindow::notification(const QString& text, Notice level) {
    const auto icon = level == Notice::Error ? style()->standardIcon(QStyle::SP_MessageBoxCritical)
        : level == Notice::Warning ? style()->standardIcon(QStyle::SP_MessageBoxWarning) : ThemeIcons::themed(QIcon(":/original/StatusInformation_16x.png"));
    const auto now = QDateTime::currentDateTime();
    // The list shows the time; the details pane shows the full message, level and date.
    auto* item = new QListWidgetItem(icon, now.toString("HH:mm:ss") + "  " + text, ui_->notificationsList);
    item->setData(Qt::UserRole, now.toString(Qt::ISODateWithMs));
    item->setData(Qt::UserRole + 1, text);
    item->setData(Qt::UserRole + 2, level == Notice::Error ? "Error" : level == Notice::Warning ? "Warning" : "Information");
    item->setToolTip(text);
    while (ui_->notificationsList->count() > 1000) delete ui_->notificationsList->takeItem(0);
    ui_->notificationsList->setCurrentItem(item); updateNotificationDetails();
}
QString MainWindow::describeSession(const SessionPane* pane) const {
    if (!pane) return {};
    const auto target = pane->target();
    QString text = target.protocol.toUpper() + " " + (target.username.isEmpty() || target.protocol == "vnc" ? QString() : target.username + "@") + target.endpoint();
    QString name;
    if (const auto doc = context(pane->document()); doc && !pane->profile().isEmpty()) name = doc->profiles().value(pane->profile()).value("name").toString();
    if (!name.isEmpty()) text += " (" + name + ")"; else if (pane->profile().isEmpty()) text += " (Quick Connect)";
    return text;
}
void MainWindow::updateNotificationDetails() {
    const auto selected = ui_->notificationsList->selectedItems(); auto* item = selected.size() == 1 ? selected.first() : nullptr;
    const auto message = item ? item->data(Qt::UserRole + 1).toString() : QString();
    ui_->notificationText->setPlainText(item ? (message.isEmpty() ? item->text() : message) : QString());
    ui_->notificationDate->setText(item ? item->data(Qt::UserRole + 2).toString() + " · " + item->data(Qt::UserRole).toString() : QString());
    ui_->notificationIcon->setVisible(item != nullptr);
    if (item) ui_->notificationIcon->setPixmap(item->icon().pixmap(16, 16));
}
void MainWindow::rebuildJumpMenu() {
    auto* menu = ui_->menuJumpTo; menu->clear();
    menu->addAction(ui_->actionNextSession); menu->addAction(ui_->actionPreviousSession); menu->addSeparator();
    for (auto* dock : {ui_->connectionsDock, ui_->configDock, ui_->notificationsDock}) {
        auto* action = menu->addAction(dock->icon(), dock->windowTitle()); action->setData(dock->objectName());
        connect(action, &QAction::triggered, this, [this, dock = QPointer<ads::CDockWidget>(dock)] {
            guard([this, dock] { if (dock) { dock->toggleView(true); dock->setAsCurrentTab(); dock->raise(); dock->widget()->setFocus(); } });
        });
    }
    menu->addSeparator();
    for (auto* panel : sessionPanels()) {
        auto* action = menu->addAction(ThemeIcons::themed(QIcon(":/original/Panel_16x.png")), panelName(panel)); action->setData(panelName(panel));
        connect(action, &QAction::triggered, this, [this, panel = QPointer<ads::CDockAreaWidget>(panel)] { guard([this, panel] { focusPanel(panel); }); });
        for (auto* pane : panelPanes(panel)) {
            auto* dock = dockOf(pane);
            auto* session = menu->addAction(dock->icon(), panelName(panel) + ": " + dock->windowTitle()); session->setData(pane->id());
            connect(session, &QAction::triggered, this, [this, pane = QPointer<SessionPane>(pane)] {
                guard([this, pane] { if (pane && isOpenPane(pane)) { focusPane(pane); pane->setFocus(); } });
            });
        }
    }
}
void MainWindow::rebuildPanelsMenu() {
    auto* menu = ui_->menuConnectionPanels; menu->clear();
    for (auto* panel : sessionPanels()) {
        auto* action = menu->addAction(ThemeIcons::themed(QIcon(":/original/Panel_16x.png")), panelName(panel));
        connect(action, &QAction::triggered, this, [this, panel = QPointer<ads::CDockAreaWidget>(panel)] { focusPanel(panel); });
    }
    if (menu->isEmpty()) menu->addAction("No open panels")->setEnabled(false);
}
void MainWindow::configureMenus() {
    for (auto* tree : QList<QTreeWidget*>{ui_->connectionTree, editor_->view()}) { tree->installEventFilter(this); tree->viewport()->installEventFilter(this); }
    ui_->notificationsList->installEventFilter(this); ui_->notificationsList->viewport()->installEventFilter(this);
    for (auto* action : {ui_->actionAddPanel, ui_->actionResetLayout, ui_->actionLockToolbars, ui_->actionFullscreen}) {
        action->setEnabled(true); action->setToolTip(action->text());
    }
    ui_->actionTreeDisconnect->setToolTip("Disconnect the saved connection; keep its tab");
    ui_->actionReconnectAll->setToolTip("Reconnect every open tab, asking for credentials one at a time");
    connect(ui_->actionReconnectAll, &QAction::triggered, this, &MainWindow::reconnectAll);
    ui_->actionHostStatus->setToolTip("Check whether the selected connection's host accepts connections on its port");
    connect(ui_->actionHostStatus, &QAction::triggered, this, &MainWindow::checkHostStatus);
    // Features CMRemote does not provide are hidden rather than shown disabled.
    for (auto* action : {ui_->actionViewScreenshots, ui_->actionHelpContents, ui_->actionWebsite, ui_->actionSupportForum, ui_->actionChat,
                         ui_->actionCommunity, ui_->actionReportIssue, ui_->actionDonate}) action->setVisible(false);
    ui_->actionDefaultProperties->setToolTip("Default properties for new connections and folders");
    ui_->actionDefaultInheritance->setToolTip("Default inheritance for new connections and folders");
    ui_->actionPropertyReset->setToolTip("Use Revert after a failed save to restore the committed value");
    ui_->menuView->setToolTipsVisible(true);
    connect(ui_->menuJumpTo, &QMenu::aboutToShow, this, &MainWindow::rebuildJumpMenu);
    rebuildJumpMenu();
    connect(ui_->menuConnectionPanels, &QMenu::aboutToShow, this, &MainWindow::rebuildPanelsMenu);
    rebuildPanelsMenu();
    ui_->actionAddPanel->setToolTip("Move the current tab into a new panel beside its own");
    connect(ui_->actionAddPanel, &QAction::triggered, this, &MainWindow::addPanel);
    connect(ui_->actionResetLayout, &QAction::triggered, this, &MainWindow::resetLayout);
    connect(ui_->actionLockToolbars, &QAction::toggled, this, [this](bool locked) {
        for (auto* bar : {ui_->quickConnectToolbar, ui_->externalToolsToolbar, ui_->multiSshToolbar})
            bar->setMovable(!locked && !(quickConnectInTitleBar_ && bar == ui_->quickConnectToolbar));
    });
    connect(ui_->actionFullscreen, &QAction::toggled, this, [this](bool full) {
        if (full) {
            applicationFullscreenState_ = windowState() & ~Qt::WindowMinimized;
            applicationFullscreenGeometry_ = isMaximized() ? normalGeometry() : geometry();
            showFullScreen();
        } else if (applicationFullscreenState_.testFlag(Qt::WindowMaximized)) showMaximized();
        else { showNormal(); if (applicationFullscreenGeometry_.isValid()) setGeometry(applicationFullscreenGeometry_); }
    });
    for (int direction : {-1, 1}) connect(direction < 0 ? ui_->actionPreviousSession : ui_->actionNextSession, &QAction::triggered, this, [this, direction] {
        const auto panes = panelPanes(currentPanel()); if (panes.isEmpty()) return;
        const int index = std::max<int>(0, panes.indexOf(currentPane()));
        focusPane(panes.at((index + panes.size() + direction) % panes.size()));
    });
    connect(ui_->actionTreeDisconnect, &QAction::triggered, this, &MainWindow::disconnectSelected);
    connect(ui_->notificationsList, &QListWidget::itemSelectionChanged, this, &MainWindow::updateNotificationDetails);
}
QAction* MainWindow::menuAction(QMenu* menu, const char* name, bool enabled, std::function<void()> command) {
    auto* source = findChild<QAction*>(QString::fromLatin1(name)); Q_ASSERT(source);
    auto* action = menu->addAction(source->icon(), source->text()); action->setObjectName(QString::fromLatin1(name));
    if (!source->shortcut().isEmpty()) { action->setShortcut(source->shortcut()); action->setShortcutVisibleInContextMenu(true); }
    action->setEnabled(enabled); action->setToolTip(enabled ? source->toolTip() : "Unavailable for this target or deferred in this milestone");
    if (command) connect(action, &QAction::triggered, this, std::move(command)); return action;
}
void MainWindow::popup(QMenu* menu, const QPoint& position) {
    menu->setToolTipsVisible(true); connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater); menu->popup(position);
}
bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (titleBarDrag(watched, event)) return true;
    if (event->type() == QEvent::Close) if (auto* floating = qobject_cast<ads::CFloatingDockContainer*>(watched)) {
        // A floating window of sessions ends them, after asking if any is connected.
        const auto docks = floating->dockWidgets();
        if (std::any_of(docks.begin(), docks.end(), [](ads::CDockWidget* dock) { return isSessionDock(dock); })) {
            event->ignore(); if (!closeApproved_) requestCloseFloating(floating); return true;
        }
    }
    if (event->type() == QEvent::DeferredDelete) if (auto* floating = qobject_cast<ads::CFloatingDockContainer*>(watched); floating && !floating->property("retired").toBool()) {
        // Qt ADS deletes a floating window once it is empty; its sessions' views may still reference it.
        floating->setProperty("retired", true);
        if (!closeApproved_ && !sessionPages().isEmpty()) { retireWindow(floating); return true; }
    }
    if (event->type() != QEvent::ContextMenu) return QMainWindow::eventFilter(watched, event);
    auto* context = static_cast<QContextMenuEvent*>(event); auto* widget = qobject_cast<QWidget*>(watched); if (!widget) return false;
    const bool keyboard = context->reason() == QContextMenuEvent::Keyboard;
    const auto location = keyboard ? widget->mapToGlobal(widget->rect().center()) : context->globalPos();
    if (watched == ui_->connectionTree || watched == ui_->connectionTree->viewport()) {
        auto* item = keyboard ? ui_->connectionTree->currentItem() : ui_->connectionTree->itemAt(ui_->connectionTree->viewport()->mapFromGlobal(location));
        if (!item) return true;
        const bool root = item->type() == DocumentItem; const auto value = item->data(0, Qt::UserRole);
        const auto owner = root ? value.value<DocumentRootReference>().document : value.value<ProfileReference>().document;
        const auto profile = root ? QString() : value.value<ProfileReference>().profile;
        guard([this, owner, profile, root, location] { if (root ? selectConnectionFile(owner) : selectConnectionProfile({owner, profile})) popup(treeMenu(profile, root), location); }); return true;
    }
    if (auto* tab = qobject_cast<ads::CDockWidgetTab*>(watched)) {
        if (auto* pane = paneOf(tab->dockWidget()); pane && isOpenPane(pane)) { focusPane(pane); popup(sessionMenu(pane), location); }
        return true;
    }
    if (auto* grid = editor_->view(); watched == grid || watched == grid->viewport()) {
        auto* item = keyboard ? grid->currentItem() : grid->itemAt(grid->viewport()->mapFromGlobal(location));
        const auto key = item ? item->data(0, ProfileEditor::KeyRole).toString() : QString();
        const auto owner = editorDocument_; const auto profile = editor_->id(); const bool root = editor_->documentRoot();
        guard([this, key, location, owner, profile, root] {
            if (!(root ? selectConnectionFile(owner) : selectConnectionProfile({owner, profile}))) return;
            editor_->setCurrentKey(key);
            popup(configMenu(), location);
        }); return true;
    }
    if (watched == ui_->notificationsList || watched == ui_->notificationsList->viewport()) {
        if (!keyboard) {
            auto* item = ui_->notificationsList->itemAt(ui_->notificationsList->viewport()->mapFromGlobal(location));
            if (!item || !item->isSelected()) ui_->notificationsList->clearSelection();
            if (item) ui_->notificationsList->setCurrentItem(item);
        }
        popup(notificationMenu(), location); return true;
    }
    return QMainWindow::eventFilter(watched, event);
}
QMenu* MainWindow::sessionMenu(SessionPane* pane) {
    auto* menu = new QMenu(this); menu->setObjectName("sessionContextMenu"); QPointer<SessionPane> target = pane;
    if (pane->target().protocol == "ssh") menuAction(menu, "actionTerminalAppearance", !closeApproved_, [this] { showTerminalAppearance(); });
    menuAction(menu, "actionTabReconnect", !transitionPending_, [this, target] { reconnectPane(target); });
    menuAction(menu, "actionDifferentCredentials", !transitionPending_ && !pane->mock() && !pane->profile().isEmpty(), [this, target] { reconnectPane(target, true); });
    menuAction(menu, "actionTabDisconnect", pane->active(), [this, target] { if (target && isOpenPane(target)) disconnectPane(target); });
    if (pane->profile().isEmpty())
        menuAction(menu, "actionSaveAsConnection", !transitionPending_, [this, target] {
            if (target && target->profile().isEmpty()) guard([this, target] { saveTemporaryToFile(target); });
        });
    auto* clipboard = menu->addAction("Share text clipboard for this session");
    clipboard->setObjectName("actionSessionClipboard"); clipboard->setCheckable(true);
    clipboard->setChecked(pane->clipboardSharingEnabled()); clipboard->setEnabled(pane->clipboardSharingAvailable());
    clipboard->setToolTip("Share Unicode text only. Off for every new connection; files and images are not shared.");
    connect(clipboard, &QAction::triggered, this, [this, target](bool enabled) {
        if (target && isOpenPane(target)) target->setClipboardSharingEnabled(enabled);
    });
    connect(pane, &SessionPane::clipboardSharingChanged, clipboard, &QAction::setChecked);
    connect(pane, &SessionPane::state, clipboard, [target, clipboard] {
        clipboard->setEnabled(target && target->clipboardSharingAvailable());
    });
    auto* capture = menu->addAction(QString("Capture mouse (%1 to release)").arg(DesktopView::captureChordText()));
    capture->setObjectName("actionSessionCaptureMouse"); capture->setEnabled(pane->pointerCaptureAvailable());
    capture->setToolTip("Lock the pointer to this desktop and send raw mouse movement, for games and 3D applications.");
    connect(capture, &QAction::triggered, this, [this, target] {
        if (!target || !isOpenPane(target)) return;
        focusPane(target);
        // After the menu closes and the window is active again.
        QTimer::singleShot(0, target, [target] { if (target) target->setPointerCaptured(true); });
    });
    auto* close = menu->addAction("Close tab"); close->setObjectName("actionTabClose");
    connect(close, &QAction::triggered, this, [this, target] { if (target) closePane(target); });
    const auto siblings = panelPanes(panelOf(pane));
    for (bool rightOnly : {false, true}) menuAction(menu, rightOnly ? "actionTabDisconnectRight" : "actionTabDisconnectOthers", siblings.size() > 1,
        [this, target, rightOnly] {
            if (!target || !isOpenPane(target)) return;
            const auto panes = panelPanes(panelOf(target)); const int keep = panes.indexOf(target);
            for (int i = 0; i < panes.size(); ++i) if (i != keep && (!rightOnly || i > keep)) disconnectPane(panes.at(i));
        });
    menuAction(menu, "actionTabRename", true, [this, target] { if (target) renamePane(target); });
    // Panels are Qt ADS dock areas: tabs also move by dragging them.
    auto* move = menu->addMenu(ThemeIcons::themed(QIcon(":/original/Panel_16x.png")), "Move to panel"); move->setObjectName("moveSessionMenu");
    const bool docked = dockOf(pane)->dockManager() == ui_->dockManager;
    for (const auto& [name, label, side] : std::initializer_list<std::tuple<const char*, const char*, int>>{
             {"actionMoveToNewPanelRight", "New panel to the right", ads::RightDockWidgetArea}, {"actionMoveToNewPanelBelow", "New panel below", ads::BottomDockWidgetArea}}) {
        auto* action = move->addAction(label); action->setObjectName(name); action->setEnabled(docked && siblings.size() > 1);
        connect(action, &QAction::triggered, this, [this, target, side] { if (target) splitPane(target, side); });
    }
    auto* floating = move->addAction("New window"); floating->setObjectName("actionMoveToNewWindow");
    floating->setEnabled(docked && !(dockOf(pane)->dockContainer()->isFloating() && siblings.size() == 1));
    connect(floating, &QAction::triggered, this, [this, target] { if (target) floatPane(target); });
    move->addSeparator();
    // General is listed even while empty: a tab can always return there.
    auto panels = sessionPanels(); if (!panels.contains(ui_->generalPanel())) panels.prepend(ui_->generalPanel());
    for (auto* panel : panels) {
        auto* action = move->addAction(panelName(panel)); action->setEnabled(docked && panel != panelOf(pane));
        connect(action, &QAction::triggered, this, [this, target, panel = QPointer<ads::CDockAreaWidget>(panel)] {
            if (target && panel) { movePaneTo(target, panel); focusPane(target); }
        });
    }
    menu->addSeparator();
    menuAction(menu, "actionTabFullscreen", !transitionPending_, [this, target] { fullscreenPane(target); });
    menuAction(menu, "actionTabMinimize", true, [this, target] { if (target && isOpenPane(target)) { focusPane(target); target->window()->showMinimized(); } });
    // Saved connections offer the same per-connection tools as the connection tree.
    if (!pane->profile().isEmpty() && !pane->mock()) {
        const ProfileReference reference{pane->document(), pane->profile()};
        const auto profile = context(reference.document) ? context(reference.document)->profiles().value(reference.profile) : QJsonObject{};
        menu->addSeparator();
        if (pane->target().protocol == "ssh")
            menuAction(menu, "actionTabTransfer", true, [this, reference] { showTransferFile(reference); })->setText("Transfer File (SSH)");
        auto* tools = menu->addMenu(ui_->actionTabExternalTools->icon(), "External Tools"); tools->setObjectName("sessionExternalToolsMenu");
        for (int i = 0; i < externalTools_.size(); ++i) {
            connect(tools->addAction(externalTools_.at(i).name), &QAction::triggered, this, [this, profile, i] {
                QString failure;
                if (ExternalTools::launch(externalTools_.at(i), profile, &failure)) notification("Launched " + externalTools_.at(i).name); else fail(failure);
            });
        }
        if (!externalTools_.isEmpty()) tools->addSeparator();
        connect(tools->addAction("Configure External Tools…"), &QAction::triggered, this, &MainWindow::showExternalTools);
    }
    return menu;
}
QMenu* MainWindow::configMenu() {
    auto* menu = new QMenu(this); menu->setObjectName("configContextMenu");
    const ProfileReference reference{editorDocument_, editor_->id()};
    const auto current = editor_->currentKey();
    if (current == "password") {
        auto selected = [this, reference](bool clear) {
            guard([this, reference, clear] {
                if (!selectConnectionProfile(reference)) return;
                if (clear) editor_->clearPassword(); else editor_->editPassword();
            });
        };
        auto* edit = menu->addAction("Edit password…"); edit->setObjectName("actionEditPassword"); edit->setEnabled(editor_->canEditPassword());
        connect(edit, &QAction::triggered, this, [selected] { selected(false); });
        auto* clear = menu->addAction("Clear password"); clear->setObjectName("actionClearPassword"); clear->setEnabled(editor_->canEditPassword());
        connect(clear, &QAction::triggered, this, [selected] { selected(true); });
    }
    if (current == "ssh_key_path") {
        auto* choose = menu->addAction("Choose SSH key file…"); choose->setObjectName("actionChooseSshKey");
        choose->setEnabled(reference.valid() && editor_->rowEditable(current) && !transitionPending_);
        connect(choose, &QAction::triggered, this, [this, reference] {
            guard([this, reference] {
                const auto doc = context(reference.document);
                if (!doc || !doc->canWrite() || closeApproved_ || !selectConnectionProfile(reference)) return;
                if (fileDialogs_.value(reference.document)) { fileDialogs_.value(reference.document)->raise(); return; }
                const auto generation = doc->status().generation; const auto revision = doc->status().revision;
                auto* picker = new QFileDialog(this, "Choose OpenSSH private key"); picker->setObjectName("sshKeyFilePicker");
                picker->setFileMode(QFileDialog::ExistingFile); picker->setNameFilter("All files (*)");
                if (testMode_) picker->setOption(QFileDialog::DontUseNativeDialog);
                picker->setAttribute(Qt::WA_DeleteOnClose); fileDialogs_.insert(reference.document, picker);
                connect(picker, &QFileDialog::fileSelected, this, [this, reference, generation, revision](const QString& path) {
                    guard([this, reference, generation, revision, path] {
                        const auto doc = context(reference.document);
                        if (!doc || !doc->canWrite() || closeApproved_ || doc->status().generation != generation || doc->status().revision != revision) {
                            fail("The owning file changed or was locked. No SSH key path was saved."); return;
                        }
                        if (selectConnectionProfile(reference)) editor_->setSshKeyPath(path);
                    });
                });
                connect(picker, &QDialog::finished, this, [this, reference] { fileDialogs_.remove(reference.document); });
                picker->open();
            });
        });
    }
    auto* inherit = menu->addAction("Use inherited value"); inherit->setEnabled(editor_->canInherit() && !transitionPending_);
    {
        const auto key = current;
        connect(inherit, &QAction::triggered, this, [this, reference, key] {
            guard([this, reference, key] {
                if (!selectConnectionProfile(reference)) return;
                if (!editor_->row(key)) return;
                editor_->setCurrentKey(key); if (editor_->canInherit()) editor_->useInherited();
            });
        });
    }
    menu->addSeparator(); menu->addAction(ui_->actionShowHelp); return menu;
}
QMenu* MainWindow::notificationMenu() {
    auto* menu = new QMenu(this); menu->setObjectName("notificationContextMenu"); auto* list = ui_->notificationsList; const bool all = list->selectedItems().isEmpty();
    QList<QPersistentModelIndex> targets;
    for (int i = 0; i < list->count(); ++i) if (all || list->item(i)->isSelected()) targets.append(list->model()->index(i, 0));
    auto* copy = menuAction(menu, "actionNotificationCopy", !targets.isEmpty(), [this, targets] {
        QStringList messages;
        for (const auto& index : targets) if (index.isValid()) {
            const auto* item = ui_->notificationsList->item(index.row());
            messages.append(item->data(Qt::UserRole).toString() + " [" + item->data(Qt::UserRole + 2).toString() + "] " + item->data(Qt::UserRole + 1).toString());
        }
        if (!messages.isEmpty()) QApplication::clipboard()->setText(messages.join("\n----------\n"));
    });
    auto* remove = menuAction(menu, "actionNotificationDelete", !targets.isEmpty(), [this, targets] {
        for (const auto& index : targets) if (index.isValid()) delete ui_->notificationsList->takeItem(index.row()); updateNotificationDetails();
    });
    if (all) { copy->setText("Copy All"); remove->setText("Delete All"); } return menu;
}
