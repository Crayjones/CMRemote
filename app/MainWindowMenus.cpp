#include "MainWindow.h"
#include "ProfileEditor.h"
#include "PropertyDelegate.h"
#include "SessionPane.h"
#include "Views.h"
#include "PanelWindow.h"
#include "SessionFullscreenController.h"
#include <QMouseEvent>
#include <QSignalBlocker>
#include "ui_test.h"
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
#include <QTabBar>
#include <QTimer>
#include <QStyle>
#include <algorithm>
#ifdef Q_OS_MACOS
void rmNativeKeepHiddenAfterFullScreen(QWidget* widget);
#endif

QList<QTabWidget*> MainWindow::groupBars() const {
    QList<QTabWidget*> result{ui_->groupTabs};
    for (const auto& window : panelWindows_) if (window) result.append(window->groups());
    return result;
}
QTabWidget* MainWindow::groupsOf(QWidget* panel) const {
    for (auto* groups : groupBars()) if (panel && groups->indexOf(panel) >= 0) return groups;
    return nullptr;
}
QMainWindow* MainWindow::hostOf(QTabWidget* groups) const {
    for (const auto& window : panelWindows_) if (window && window->groups() == groups) return window;
    return const_cast<MainWindow*>(this);
}
PanelWindow* MainWindow::panelWindowOf(QWidget* widget) const {
    for (const auto& window : panelWindows_) if (window && widget && (window == widget || window->isAncestorOf(widget))) return window;
    return nullptr;
}
SessionFullscreenController* MainWindow::fullscreenFor(QWidget* widget) const {
    if (auto* window = panelWindowOf(widget)) return window->fullscreen();
    return nullptr;
}
void MainWindow::exitFullscreenFor(SessionPane* pane) {
    // Exiting can return the session and retire its window, so walk a copy.
    const auto windows = panelWindows_;
    for (const auto& window : windows) if (window && window->fullscreen()->target() == pane) window->fullscreen()->exit();
}
bool MainWindow::isFullscreenWindow(const QWidget* window) const {
    return std::any_of(fullscreenWindows_.begin(), fullscreenWindows_.end(), [window](const FullscreenWindow& entry) { return window && entry.window == window; });
}
QMainWindow* MainWindow::sessionFullscreenWindow() const {
    for (const auto& entry : fullscreenWindows_) if (entry.window && entry.window->fullscreen()->active()) return entry.window;
    return nullptr;
}
QList<QTabWidget*> MainWindow::sessionPanels() const {
    QList<QTabWidget*> result;
    // A page can be null transiently while a QTabWidget removes it.
    for (auto* groups : groupBars()) for (int i = 0; i < groups->count(); ++i)
        if (auto* panel = groups->widget(i)) if (auto* tabs = panel->findChild<QTabWidget*>()) result.append(tabs);
    return result;
}
QList<SessionPane*> MainWindow::sessionPages() const {
    QList<SessionPane*> result;
    for (auto* tabs : sessionPanels()) for (int i = 0; i < tabs->count(); ++i)
        if (auto* pane = qobject_cast<SessionPane*>(tabs->widget(i))) result.append(pane);
    return result;
}
QTabWidget* MainWindow::currentSessions() const {
    // New sessions open in the window whose sessions were used last.
    auto* groups = lastWindow_ && lastWindow_->isVisible() && panelWindows_.contains(lastWindow_) && !isFullscreenWindow(lastWindow_) ?
        lastWindow_->groups() : ui_->groupTabs;
    auto* panel = groups->currentWidget();
    if (!panel) { groups = ui_->groupTabs; panel = groups->currentWidget(); }
    return panel ? panel->findChild<QTabWidget*>() : nullptr;
}
QTabWidget* MainWindow::ownerOf(SessionPane* pane) const {
    if (!pane) return nullptr;
    for (auto* tabs : sessionPanels()) if (tabs->indexOf(pane) >= 0) return tabs;
    return nullptr;
}
void MainWindow::focusPane(SessionPane* pane) {
    auto* tabs = ownerOf(pane); if (!tabs) return;
    auto* panel = tabs->parentWidget(); auto* groups = groupsOf(panel);
    if (groups) groups->setCurrentWidget(panel);
    tabs->setCurrentWidget(pane);
    if (auto* window = panelWindowOf(tabs)) { lastWindow_ = window; window->raise(); window->activateWindow(); }
    else lastWindow_.clear();
    updateActions();
}
void MainWindow::configureTabs(QTabWidget* tabs) {
    tabs->setDocumentMode(true); tabs->setTabsClosable(true); tabs->setMovable(true);
    tabs->tabBar()->installEventFilter(this);
    connect(tabs, &QTabWidget::currentChanged, this, [this, tabs] {
        if (auto* fullscreen = fullscreenFor(tabs); fullscreen && fullscreen->active() && fullscreen->target() && ownerOf(fullscreen->target()) == tabs)
            fullscreen->checkSelection(tabs);
        updateActions();
    });
    connect(tabs, &QTabWidget::tabCloseRequested, this, [this, tabs](int index) {
        closePane(qobject_cast<SessionPane*>(tabs->widget(index)));
    });
}
void MainWindow::configureGroups(QTabWidget* groups) {
    groups->tabBar()->installEventFilter(this);
    connect(groups, &QTabWidget::currentChanged, this, [this, groups] {
        if (auto* fullscreen = fullscreenFor(groups); fullscreen && fullscreen->active()) {
            auto* panel = groups->currentWidget(); fullscreen->checkSelection(panel ? panel->findChild<QTabWidget*>() : nullptr);
        }
        updateActions();
    });
}
void MainWindow::closePane(SessionPane* pane) {
    auto* tabs = ownerOf(pane); if (!tabs) return;
    // A fullscreen session closes in its own window instead of returning first.
    pane->setProperty("closingPane", true);
    disconnectPane(pane);
    sessionCore(pane->document()).close_session(pane->id().toUtf8().toStdString());
    tabs->removeTab(tabs->indexOf(pane)); delete pane; updateActions();
}
void MainWindow::closeAllSessions() {
    if (pollTimer_) pollTimer_->stop();
    cancelCredentials();
    for (auto* pane : sessionPages()) closePane(pane);
    // Every session is gone, so no view still references a window's GPU context.
    for (const auto& window : panelWindows_ + retiredWindows_) if (window) { window->hide(); window->deleteLater(); }
    panelWindows_.clear(); retiredWindows_.clear();
}
void MainWindow::movePane(QPointer<SessionPane> pane, QPointer<QTabWidget> destination) {
    auto* source = ownerOf(pane);
    if (!source || !destination || !sessionPanels().contains(destination) || source == destination) return;
    movePaneTo(pane, destination); focusPane(pane);
}
void MainWindow::movePaneTo(SessionPane* pane, QTabWidget* destination, int index) {
    auto* source = ownerOf(pane); if (!source || !destination) return;
    exitFullscreenFor(pane);
    if (credentialPage_ == pane) cancelCredentials();
    const int from = source->indexOf(pane); const auto caption = source->tabText(from); const auto icon = source->tabIcon(from);
    if (source == destination) { source->tabBar()->moveTab(from, index < 0 ? source->count() - 1 : qMin(index, source->count() - 1)); return; }
    source->removeTab(from);
    destination->insertTab(index < 0 ? destination->count() : qMin(index, destination->count()), pane, icon, caption);
    destination->setCurrentWidget(pane);
}
QWidget* MainWindow::createPanel(QTabWidget* groups, const QString& requested) {
    const auto name = requested.isEmpty() ? QString("Panel %1").arg(nextPanel_++) : requested;
    auto* panel = new QWidget(groups); auto* layout = new QVBoxLayout(panel); layout->setContentsMargins(0, 0, 0, 0);
    auto* tabs = new QTabWidget(panel); tabs->setObjectName("panelSessionTabs"); configureTabs(tabs); layout->addWidget(tabs);
    auto* action = ui_->menuConnectionPanels->addAction(name); panel->setProperty("panelAction", QVariant::fromValue(action));
    connect(action, &QAction::triggered, this, [this, panel = QPointer<QWidget>(panel)] {
        if (auto* groups = groupsOf(panel)) { groups->setCurrentWidget(panel); hostOf(groups)->raise(); hostOf(groups)->activateWindow(); }
    });
    connect(panel, &QObject::destroyed, action, &QObject::deleteLater);
    groups->setCurrentIndex(groups->addTab(panel, QIcon(":/original/Panel_16x.png"), name));
    return panel;
}
void MainWindow::addPanel() {
    if (closeApproved_) return;
    // A new panel is only reachable through its tab, so adding one shows the panel row.
    ui_->actionViewPanels->setChecked(true); createPanel(ui_->groupTabs);
}
void MainWindow::movePanel(QWidget* panel, QTabWidget* destination, int index) {
    auto* source = groupsOf(panel); if (!source || !destination || panel->property("isGeneralPanel").toBool()) return;
    if (auto* tabs = panel->findChild<QTabWidget*>()) for (int i = 0; i < tabs->count(); ++i) exitFullscreenFor(qobject_cast<SessionPane*>(tabs->widget(i)));
    const int from = source->indexOf(panel); const auto caption = source->tabText(from); const auto icon = source->tabIcon(from);
    if (source == destination) { source->tabBar()->moveTab(from, index < 0 ? source->count() - 1 : qMin(index, source->count() - 1)); return; }
    source->removeTab(from);
    destination->insertTab(index < 0 ? destination->count() : qMin(index, destination->count()), panel, icon, caption);
    destination->setCurrentWidget(panel);
    if (auto* window = panelWindowOf(source); window && !source->count()) retireWindow(window);
}
void MainWindow::closeEmptyPanel(QWidget* panel) {
    // A panel emptied by dragging its last tab away disappears, like a browser window.
    auto* groups = groupsOf(panel); auto* tabs = panel ? panel->findChild<QTabWidget*>() : nullptr;
    if (!groups || !tabs || tabs->count() || panel->property("isGeneralPanel").toBool()) return;
    if (credentialOwner_ == tabs) cancelCredentials();
    if (auto* action = panel->property("panelAction").value<QAction*>()) { ui_->menuConnectionPanels->removeAction(action); action->setEnabled(false); }
    groups->removeTab(groups->indexOf(panel)); panel->deleteLater();
    if (auto* window = panelWindowOf(groups); window && !groups->count()) retireWindow(window);
    updateActions();
}
void MainWindow::closePanel(QWidget* panel) {
    auto* owner = groupsOf(panel);
    if (!panel || panel->property("isGeneralPanel").toBool() || !owner) return;
    QPointer<QWidget> target = panel;
    auto remove = [this, target] {
        auto* groups = groupsOf(target);
        if (!target || target->property("isGeneralPanel").toBool() || !groups) return;
        auto* tabs = target->findChild<QTabWidget*>();
        if (credentialOwner_ == tabs) cancelCredentials();
        while (tabs && tabs->count()) closePane(qobject_cast<SessionPane*>(tabs->widget(0)));
        if (auto* action = target->property("panelAction").value<QAction*>()) {
            ui_->menuConnectionPanels->removeAction(action); action->setEnabled(false);
        }
        groups->removeTab(groups->indexOf(target)); target->deleteLater();
        if (auto* window = panelWindowOf(groups); window && !groups->count()) retireWindow(window);
        updateActions();
    };
    auto* tabs = panel->findChild<QTabWidget*>();
    if (testMode_ || !tabs || !tabs->count()) { remove(); return; }
    auto* dialog = new QMessageBox(QMessageBox::Question, "Close panel", "Close this panel and stop all its sessions?",
                                  QMessageBox::Yes | QMessageBox::Cancel, hostOf(owner));
    dialog->setObjectName("closePanel"); dialog->setTextFormat(Qt::PlainText); dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel); dialog->setWindowModality(Qt::WindowModal); dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [remove](int answer) { if (answer == QMessageBox::Yes) remove(); }); dialog->open();
}
PanelWindow* MainWindow::createPanelWindow(QPoint position, QSize size) {
    auto* window = new PanelWindow(nextWindow_++); panelWindows_.append(window);
    configureGroups(window->groups());
    connect(window->fullscreen(), &SessionFullscreenController::closeRequested, this, &MainWindow::closePane);
    connect(window, &PanelWindow::closeRequested, this, &MainWindow::requestCloseWindow);
    connect(window, &PanelWindow::activated, this, [this](PanelWindow* window) { if (!isFullscreenWindow(window)) lastWindow_ = window; updateActions(); });
    connect(window, &PanelWindow::newPanelRequested, this, [this](PanelWindow* window) { createPanel(window->groups()); });
    connect(window, &PanelWindow::docksRequested, this, &MainWindow::lendDocks);
    window->resize(size.expandedTo(QSize(640, 480))); window->move(position);
    return window;
}
void MainWindow::requestCloseWindow(PanelWindow* window) {
    if (!window || !panelWindows_.contains(window)) return;
    QList<QPointer<SessionPane>> panes; bool active = false;
    for (auto* pane : sessionPages()) if (panelWindowOf(pane) == window) { panes.append(pane); active = active || pane->active(); }
    QPointer<PanelWindow> target = window;
    auto close = [this, target, panes] {
        if (!target) return;
        for (const auto& pane : panes) if (pane) closePane(pane);
        retireWindow(target);
    };
    if (testMode_ || !active) { close(); return; }
    auto* dialog = new QMessageBox(QMessageBox::Question, "Close window", QString("Close this window and end its %1 session(s)?").arg(panes.size()),
                                  QMessageBox::Yes | QMessageBox::Cancel, window);
    dialog->setObjectName("closeWindow"); dialog->setTextFormat(Qt::PlainText); dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel); dialog->setWindowModality(Qt::WindowModal); dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [close](int answer) { if (answer == QMessageBox::Yes) close(); }); dialog->open();
}
void MainWindow::retireWindow(PanelWindow* window) {
    if (!window || !panelWindows_.contains(window)) return;
    if (docksWindow_ == window) lendDocks(window, false);
    fullscreenWindows_.removeIf([window](const FullscreenWindow& entry) { return entry.window == window; });
    window->fullscreen()->exit();
    // Close any panels left behind (they are empty or their sessions just ended).
    while (window->groups()->count()) {
        auto* panel = window->groups()->widget(0);
        if (auto* action = panel->property("panelAction").value<QAction*>()) { ui_->menuConnectionPanels->removeAction(action); action->setEnabled(false); }
        window->groups()->removeTab(0); panel->deleteLater();
    }
    panelWindows_.removeAll(window); if (lastWindow_ == window) lastWindow_.clear();
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
    updateActions();
}
void MainWindow::lendDocks(PanelWindow* window, bool here) {
    QMainWindow* target = here && window ? static_cast<QMainWindow*>(window) : this;
    if (docksWindow_ && docksWindow_ != window) docksWindow_->setDocksHere(false);
    docksWindow_ = here ? window : nullptr;
    if (window) window->setDocksHere(here);
    target->addDockWidget(Qt::LeftDockWidgetArea, ui_->connectionsDock);
    target->splitDockWidget(ui_->connectionsDock, ui_->configDock, Qt::Vertical);
    ui_->connectionsDock->show(); ui_->configDock->show();
    if (here && window) lastWindow_ = window;
}
void MainWindow::renamePage(QTabWidget* tabs, QWidget* page, bool panel) {
    if (!tabs || !page || tabs->indexOf(page) < 0) return;
    QPointer<QTabWidget> owner = tabs; QPointer<QWidget> target = page;
    auto* dialog = new QInputDialog(this); dialog->setObjectName("renamePage"); dialog->setWindowTitle(panel ? "Rename panel" : "Rename tab");
    dialog->setLabelText("Name:"); dialog->setTextValue(tabs->tabText(tabs->indexOf(page)));
    dialog->setWindowModality(Qt::WindowModal); dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QInputDialog::textValueSelected, this, [this, owner, target, panel](const QString& value) {
        if (!target || closeApproved_ || value.trimmed().isEmpty()) return;
        auto* tabs = panel ? owner.data() : ownerOf(qobject_cast<SessionPane*>(target.data()));
        if (!tabs || tabs->indexOf(target) < 0) return;
        tabs->setTabText(tabs->indexOf(target), value.trimmed());
        if (panel) if (auto* action = target->property("panelAction").value<QAction*>()) action->setText(value.trimmed());
    }); dialog->open();
}
void MainWindow::fullscreenPane(QPointer<SessionPane> pane) {
    // The session moves to its own fullscreen window, leaving the window it came
    // from usable; exiting fullscreen returns it to the same tab position.
    guard([this, pane] {
        auto* owner = ownerOf(pane); if (!pane || !owner) return;
        if (auto* current = fullscreenFor(owner); current && current->active() && current->target() == pane) { current->exit(); return; }
        exitFullscreenFor(pane);
        owner = ownerOf(pane); auto* host = owner ? hostOf(groupsOf(owner->parentWidget())) : nullptr; if (!host) return;
        const auto caption = owner->tabText(owner->indexOf(pane));
        auto* window = createPanelWindow(host->geometry().topLeft(), host->size());
        fullscreenWindows_.append({window, owner, owner->indexOf(pane)});
        auto* panel = createPanel(window->groups(), caption);
        if (auto* action = panel->property("panelAction").value<QAction*>()) { ui_->menuConnectionPanels->removeAction(action); action->setEnabled(false); }
        auto* tabs = panel->findChild<QTabWidget*>(); movePaneTo(pane, tabs);
        connect(window->fullscreen(), &SessionFullscreenController::exited, this,
                [this, target = QPointer<PanelWindow>(window)](SessionPane* left) { returnFromFullscreen(target, left); });
        window->show(); window->fullscreen()->enter(pane, tabs, caption);
        updateActions();
    });
}
void MainWindow::returnFromFullscreen(QPointer<PanelWindow> window, QPointer<SessionPane> pane) {
    const auto entry = std::find_if(fullscreenWindows_.begin(), fullscreenWindows_.end(), [&](const FullscreenWindow& item) { return item.window == window; });
    if (entry == fullscreenWindows_.end()) return;
    const auto origin = entry->origin; const int index = entry->index; fullscreenWindows_.erase(entry);
    const bool closing = !pane || pane->property("closingPane").toBool();
    if (!closing && panelWindowOf(pane) == window) {
        // The original panel may have closed meanwhile; General always exists.
        auto* destination = origin && sessionPanels().contains(origin) ? origin.data() : ui_->sessionTabs;
        movePaneTo(pane, destination, index); focusPane(pane);
        if (auto* host = hostOf(groupsOf(destination->parentWidget()))) { host->raise(); host->activateWindow(); }
    }
    auto retire = [this, window] { if (window && panelWindows_.contains(window)) retireWindow(window); };
    // A closing or destroyed session is still leaving the window; retire it after.
    if (closing) QTimer::singleShot(0, this, retire); else retire();
}
void MainWindow::resetLayout() {
    if (docksWindow_) lendDocks(docksWindow_, false);
    for (auto* dock : {ui_->connectionsDock, ui_->configDock, ui_->notificationsDock}) { dock->setFloating(false); removeDockWidget(dock); }
    addDockWidget(Qt::LeftDockWidgetArea, ui_->connectionsDock); splitDockWidget(ui_->connectionsDock, ui_->configDock, Qt::Vertical);
    addDockWidget(Qt::BottomDockWidgetArea, ui_->notificationsDock);
    ui_->connectionsDock->show(); ui_->configDock->show(); ui_->notificationsDock->hide();
    resizeDocks({ui_->connectionsDock}, {226}, Qt::Horizontal); resizeDocks({ui_->connectionsDock, ui_->configDock}, {210, 250}, Qt::Vertical);
    resizeDocks({ui_->notificationsDock}, {95}, Qt::Vertical);
    for (auto* toolbar : {ui_->quickConnectToolbar, ui_->externalToolsToolbar, ui_->multiSshToolbar}) {
        removeToolBar(toolbar); addToolBar(Qt::TopToolBarArea, toolbar);
        toolbar->setMovable(!ui_->actionLockToolbars->isChecked() && !(quickConnectInTitleBar_ && toolbar == ui_->quickConnectToolbar));
    }
    ui_->quickConnectToolbar->show(); ui_->externalToolsToolbar->hide(); ui_->multiSshToolbar->hide();
}
void MainWindow::notification(const QString& text, Notice level) {
    const auto icon = level == Notice::Error ? style()->standardIcon(QStyle::SP_MessageBoxCritical)
        : level == Notice::Warning ? style()->standardIcon(QStyle::SP_MessageBoxWarning) : QIcon(":/original/StatusInformation_16x.png");
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
        auto* action = menu->addAction(dock->windowIcon(), dock->windowTitle()); action->setData(dock->objectName());
        connect(action, &QAction::triggered, this, [this, dock = QPointer<QDockWidget>(dock)] {
            guard([this, dock] { if (dock) { dock->show(); dock->raise(); dock->widget()->setFocus(); } });
        });
    }
    menu->addSeparator();
    for (auto* groups : groupBars()) for (int i = 0; i < groups->count(); ++i) {
        auto* panel = groups->widget(i); if (!panel) continue; auto* tabs = panel->findChild<QTabWidget*>();
        const auto prefix = groups == ui_->groupTabs ? QString() : hostOf(groups)->windowTitle().section("— ", 1) + " · ";
        auto* action = menu->addAction(groups->tabIcon(i), prefix + groups->tabText(i)); action->setData(QVariant::fromValue(panel));
        connect(action, &QAction::triggered, this, [this, panel = QPointer<QWidget>(panel)] {
            guard([this, panel] {
                auto* groups = groupsOf(panel); if (!panel || !groups) return;
                groups->setCurrentWidget(panel); hostOf(groups)->raise(); hostOf(groups)->activateWindow();
                if (auto* tabs = panel->findChild<QTabWidget*>()) tabs->setFocus();
            });
        });
        if (!tabs) continue;
        for (int j = 0; j < tabs->count(); ++j) {
            auto* pane = qobject_cast<SessionPane*>(tabs->widget(j)); if (!pane) continue;
            auto* session = menu->addAction(tabs->tabIcon(j), prefix + groups->tabText(i) + ": " + tabs->tabText(j)); session->setData(pane->id());
            connect(session, &QAction::triggered, this, [this, pane = QPointer<SessionPane>(pane)] {
                guard([this, pane] { if (pane && ownerOf(pane)) { focusPane(pane); pane->setFocus(); } });
            });
        }
    }
}
void MainWindow::configureMenus() {
    ui_->generalPanel->setProperty("panelAction", QVariant::fromValue(ui_->actionGeneralPanel));
    configureGroups(ui_->groupTabs); ui_->groupTabs->setMovable(true);
    for (auto* tree : QList<QTreeWidget*>{ui_->connectionTree, ui_->configTree}) { tree->installEventFilter(this); tree->viewport()->installEventFilter(this); }
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
        auto* tabs = currentSessions(); if (tabs && tabs->count()) tabs->setCurrentIndex((tabs->currentIndex() + tabs->count() + direction) % tabs->count());
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
    if (auto* bar = qobject_cast<QTabBar*>(watched); bar && tabDragEvent(bar, event)) return true;
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
    if (auto* bar = qobject_cast<QTabBar*>(watched)) {
        auto* tabs = qobject_cast<QTabWidget*>(bar->parentWidget()); if (!tabs) return false;
        const int index = keyboard ? tabs->currentIndex() : bar->tabAt(context->pos()); if (index < 0) return true;
        if (groupBars().contains(tabs)) popup(groupMenu(tabs->widget(index)), location);
        else if (auto* pane = qobject_cast<SessionPane*>(tabs->widget(index))) popup(sessionMenu(pane), location);
        return true;
    }
    if (watched == ui_->configTree || watched == ui_->configTree->viewport()) {
        auto* item = keyboard ? ui_->configTree->currentItem() : ui_->configTree->itemAt(ui_->configTree->viewport()->mapFromGlobal(location));
        const auto key = item ? item->data(0, PropertyDelegate::KeyRole).toString() : QString();
        const auto owner = editorDocument_; const auto profile = editor_->id(); const bool root = editor_->documentRoot();
        guard([this, key, location, owner, profile, root] {
            if (!(root ? selectConnectionFile(owner) : selectConnectionProfile({owner, profile}))) return;
            for (QTreeWidgetItemIterator it(ui_->configTree); *it; ++it)
                if ((*it)->data(0, PropertyDelegate::KeyRole).toString() == key) { ui_->configTree->setCurrentItem(*it, 1); break; }
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
    menuAction(menu, "actionTabDisconnect", pane->active(), [this, target] { if (target && ownerOf(target)) disconnectPane(target); });
    if (pane->profile().isEmpty())
        menuAction(menu, "actionSaveAsConnection", !transitionPending_, [this, target] {
            if (target && target->profile().isEmpty()) guard([this, target] { saveTemporaryToFile(target); });
        });
    auto* clipboard = menu->addAction("Share text clipboard for this session");
    clipboard->setObjectName("actionSessionClipboard"); clipboard->setCheckable(true);
    clipboard->setChecked(pane->clipboardSharingEnabled()); clipboard->setEnabled(pane->clipboardSharingAvailable());
    clipboard->setToolTip("Share Unicode text only. Off for every new connection; files and images are not shared.");
    connect(clipboard, &QAction::triggered, this, [this, target](bool enabled) {
        if (target && ownerOf(target)) target->setClipboardSharingEnabled(enabled);
    });
    connect(pane, &SessionPane::clipboardSharingChanged, clipboard, &QAction::setChecked);
    connect(pane, &SessionPane::state, clipboard, [target, clipboard] {
        clipboard->setEnabled(target && target->clipboardSharingAvailable());
    });
    auto* capture = menu->addAction(QString("Capture mouse (%1 to release)").arg(DesktopView::captureChordText()));
    capture->setObjectName("actionSessionCaptureMouse"); capture->setEnabled(pane->pointerCaptureAvailable());
    capture->setToolTip("Lock the pointer to this desktop and send raw mouse movement, for games and 3D applications.");
    connect(capture, &QAction::triggered, this, [this, target] {
        if (!target || !ownerOf(target)) return;
        focusPane(target);
        // After the menu closes and the window is active again.
        QTimer::singleShot(0, target, [target] { if (target) target->setPointerCaptured(true); });
    });
    auto* close = menu->addAction("Close tab"); close->setObjectName("actionTabClose");
    connect(close, &QAction::triggered, this, [this, target] { if (target) closePane(target); });
    for (bool rightOnly : {false, true}) menuAction(menu, rightOnly ? "actionTabDisconnectRight" : "actionTabDisconnectOthers", ownerOf(pane)->count() > 1,
        [this, target, rightOnly] {
            auto* tabs = ownerOf(target); if (!tabs) return; const int keep = tabs->indexOf(target);
            for (int i = 0; i < tabs->count(); ++i) if (i != keep && (!rightOnly || i > keep)) disconnectPane(qobject_cast<SessionPane*>(tabs->widget(i)));
        });
    menuAction(menu, "actionTabRename", true, [this, target] { if (target) renamePage(ownerOf(target), target, false); });
    auto* move = menu->addMenu("Move to panel"); move->setObjectName("moveSessionMenu");
    for (auto* groups : groupBars()) for (int i = 0; i < groups->count(); ++i) {
        if (!groups->widget(i)) continue;
        QPointer<QTabWidget> tabs = groups->widget(i)->findChild<QTabWidget*>();
        const auto prefix = groups == ui_->groupTabs ? QString() : hostOf(groups)->windowTitle().section("— ", 1) + " · ";
        auto* action = move->addAction(prefix + groups->tabText(i)); action->setEnabled(tabs && tabs != ownerOf(pane));
        connect(action, &QAction::triggered, this, [this, target, tabs] {
            auto* source = target ? ownerOf(target) : nullptr; movePane(target, tabs);
            if (source && !source->count()) closeEmptyPanel(source->parentWidget());
        });
    }
    menu->addSeparator();
    menuAction(menu, "actionTabFullscreen", !transitionPending_, [this, target] { fullscreenPane(target); });
    menuAction(menu, "actionTabMinimize", true, [this, target] { if (target && ownerOf(target)) { focusPane(target); showMinimized(); } });
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
QMenu* MainWindow::groupMenu(QWidget* panel) {
    auto* menu = new QMenu(this); menu->setObjectName("groupContextMenu"); QPointer<QWidget> target = panel;
    menuAction(menu, "actionPanelRename", true, [this, target] { if (target) renamePage(groupsOf(target), target, true); });
    menuAction(menu, "actionPanelClose", !panel->property("isGeneralPanel").toBool(), [this, target] { if (target) closePanel(target); });
    return menu;
}
QMenu* MainWindow::configMenu() {
    auto* menu = new QMenu(this); menu->setObjectName("configContextMenu");
    const ProfileReference reference{editorDocument_, editor_->id()};
    const auto* row = ui_->configTree->currentItem();
    if (row && row->data(0, PropertyDelegate::KeyRole).toString() == "password") {
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
    if (row && row->data(0, PropertyDelegate::KeyRole).toString() == "ssh_key_path") {
        auto* choose = menu->addAction("Choose SSH key file…"); choose->setObjectName("actionChooseSshKey");
        choose->setEnabled(reference.valid() && (row->flags() & Qt::ItemIsEditable) && !transitionPending_);
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
        const auto key = row ? row->data(0, PropertyDelegate::KeyRole).toString() : QString();
        connect(inherit, &QAction::triggered, this, [this, reference, key] {
            guard([this, reference, key] {
                if (!selectConnectionProfile(reference)) return;
                for (QTreeWidgetItemIterator it(ui_->configTree); *it; ++it)
                    if ((*it)->data(0, PropertyDelegate::KeyRole).toString() == key) {
                        ui_->configTree->setCurrentItem(*it, 1); if (editor_->canInherit()) editor_->useInherited(); break;
                    }
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

MainWindow::DropTarget MainWindow::dropTarget(QPoint global, QWidget* exclude) const {
    for (auto* groups : groupBars()) {
        auto* host = hostOf(groups);
        if (host == exclude || !host->isVisible() || host->isMinimized()) continue;
        auto* bar = groups->tabBar();
        const QRect panels(QPoint(groups->mapToGlobal(QPoint(0, 0)).x(), bar->mapToGlobal(QPoint(0, 0)).y()), QSize(groups->width(), qMax(bar->height(), 20)));
        if (bar->isVisible() && panels.contains(global)) return {groups, nullptr, bar->tabAt(bar->mapFromGlobal(global))};
        auto* panel = groups->currentWidget(); auto* tabs = panel ? panel->findChild<QTabWidget*>() : nullptr;
        if (!tabs) continue;
        auto* strip = tabs->tabBar();
        // The whole width of the session tab row accepts drops, even past the last tab.
        const QRect sessions(QPoint(tabs->mapToGlobal(QPoint(0, 0)).x(), strip->mapToGlobal(QPoint(0, 0)).y()), QSize(tabs->width(), qMax(strip->height(), 24)));
        if (sessions.contains(global)) return {nullptr, tabs, strip->tabAt(strip->mapFromGlobal(global))};
    }
    return {};
}
void MainWindow::showDropHighlight(const DropTarget& target) {
    QTabWidget* area = target.sessions ? target.sessions.data() : target.groups.data();
    if (!area) { if (dropHighlight_) dropHighlight_->hide(); return; }
    // A child of the target window, as Qt's dock gap is, so the dragged window stays above it.
    auto* host = area->window();
    if (!dropHighlight_) { dropHighlight_ = new QRubberBand(QRubberBand::Rectangle, host); dropHighlight_->setObjectName("tabDropHighlight"); }
    else if (dropHighlight_->parentWidget() != host) dropHighlight_->setParent(host);
    QRect rect(area->mapTo(host, QPoint(0, 0)), area->size());
    if (!target.sessions) {
        // Over the panel row only the panel tab it joins, or the row past the last tab, lights up.
        auto* bar = area->tabBar();
        rect = target.index >= 0 ? QRect(bar->mapTo(host, bar->tabRect(target.index).topLeft()), bar->tabRect(target.index).size())
                                 : QRect(QPoint(rect.x(), bar->mapTo(host, QPoint(0, 0)).y()), QSize(area->width(), qMax(bar->height(), 20)));
    }
    dropHighlight_->setGeometry(rect);
    dropHighlight_->show(); dropHighlight_->raise();
}
bool MainWindow::tabDragEvent(QTabBar* bar, QEvent* event) {
    const auto type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove && type != QEvent::MouseButtonRelease) return false;
    if (drag_.synthetic) return false;
    auto* tabs = qobject_cast<QTabWidget*>(bar->parentWidget()); if (!tabs) return false;
    const bool panelBar = groupBars().contains(tabs);
    if (!panelBar && !sessionPanels().contains(tabs)) return false;
    auto* mouse = static_cast<QMouseEvent*>(event); const auto global = mouse->globalPosition().toPoint();
    if (type == QEvent::MouseButtonPress) {
        if (mouse->button() != Qt::LeftButton) return false;
        // Pressing a tab marks its window as the one in use for new sessions.
        if (auto* window = panelWindowOf(bar)) lastWindow_ = window; else lastWindow_.clear();
        const int index = bar->tabAt(mouse->position().toPoint()); auto* item = index >= 0 ? tabs->widget(index) : nullptr;
        if (panelBar && item && item->property("isGeneralPanel").toBool()) item = nullptr;
        drag_ = TabDrag{}; drag_.bar = bar; drag_.item = item; drag_.panel = panelBar; drag_.sourcePanel = panelBar ? nullptr : tabs->parentWidget();
        return false;
    }
    if (drag_.bar != bar || !drag_.item) return false;
    if (type == QEvent::MouseButtonRelease) {
        if (!drag_.detached) { drag_ = TabDrag{}; return false; }
        finishDrag(global); return true;
    }
    if (!(mouse->buttons() & Qt::LeftButton) || closeApproved_) return false;
    if (!drag_.detached) {
        // Inside the strip the bar reorders tabs itself; leaving it vertically starts a drag.
        const int y = mouse->position().toPoint().y(), margin = 20;
        const auto hovered = dropTarget(global, nullptr);
        const bool otherStrip = (hovered.groups && hovered.groups != tabs) || (hovered.sessions && hovered.sessions != tabs);
        if (!otherStrip && y >= -margin && y <= bar->height() + margin) return false;
        drag_.detached = true; drag_.synthetic = true;
        QMouseEvent release(QEvent::MouseButtonRelease, mouse->position(), mouse->globalPosition(), Qt::LeftButton, Qt::NoButton, mouse->modifiers());
        QApplication::sendEvent(bar, &release); drag_.synthetic = false;
        QApplication::setOverrideCursor(Qt::ClosedHandCursor);
    }
    if (drag_.floating) { drag_.floating->move(global - drag_.offset); showDropHighlight(dropTarget(global, drag_.floating)); return true; }
    const auto target = dropTarget(global, nullptr); showDropHighlight(target);
    if (target.groups || target.sessions) return true; // Over a tab strip: the drop decides.
    // Tear off. A dragged-out window holding only this item moves as a whole.
    auto* pane = drag_.panel ? nullptr : qobject_cast<SessionPane*>(drag_.item.data());
    auto* sourceTabs = pane ? ownerOf(pane) : nullptr;
    if (!drag_.panel && !sourceTabs) return true;
    auto* sourceGroups = drag_.panel ? groupsOf(drag_.item) : groupsOf(sourceTabs->parentWidget());
    auto* sourceWindow = panelWindowOf(sourceGroups);
    if (sourceWindow && sourceGroups->count() == 1 && (drag_.panel || sourceTabs->count() == 1)) {
        drag_.floating = sourceWindow; drag_.offset = global - sourceWindow->pos(); return true;
    }
    drag_.offset = QPoint(80, 40);
    auto* window = createPanelWindow(global - drag_.offset, hostOf(sourceGroups)->size() * 0.8);
    if (drag_.panel) movePanel(drag_.item, window->groups());
    else movePaneTo(pane, createPanel(window->groups())->findChild<QTabWidget*>());
    window->show(); window->raise(); drag_.floating = window;
    return true;
}
void MainWindow::finishDrag(QPoint global) {
    QApplication::restoreOverrideCursor(); showDropHighlight({});
    const auto drag = drag_; drag_ = TabDrag{};
    if (!drag.item || closeApproved_) return;
    const auto target = dropTarget(global, drag.floating);
    if (target.groups || target.sessions) {
        if (drag.panel) {
            auto* groups = target.groups ? target.groups.data() : groupsOf(target.sessions->parentWidget());
            movePanel(drag.item, groups, target.groups ? target.index : -1);
            hostOf(groups)->raise(); hostOf(groups)->activateWindow();
        } else if (auto* pane = qobject_cast<SessionPane*>(drag.item.data())) {
            QTabWidget* destination = target.sessions; int index = target.index;
            if (!destination) {
                if (target.index >= 0) { destination = target.groups->widget(target.index)->findChild<QTabWidget*>(); index = -1; }
                else destination = createPanel(target.groups)->findChild<QTabWidget*>();
            }
            auto* source = ownerOf(pane);
            movePaneTo(pane, destination, index); focusPane(pane);
            if (source && source != destination && !source->count()) closeEmptyPanel(source->parentWidget());
        }
        // A window the item was carried in disappears once it is empty.
        if (drag.floating && panelWindows_.contains(drag.floating)) {
            const auto groups = drag.floating->groups();
            for (int i = groups->count() - 1; i >= 0; --i) closeEmptyPanel(groups->widget(i));
            if (panelWindows_.contains(drag.floating) && !groups->count()) retireWindow(drag.floating);
        }
    } else if (drag.floating) {
        if (drag.sourcePanel && drag.floating->groups()->indexOf(drag.sourcePanel) < 0) closeEmptyPanel(drag.sourcePanel);
        drag.floating->raise(); drag.floating->activateWindow(); lastWindow_ = drag.floating;
    }
    updateActions();
}
