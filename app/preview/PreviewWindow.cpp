#include "PreviewWindow.h"
#include "Branding.h"
#include "ProfileEditor.h"
#include "QuickConnect.h"
#include <QSignalBlocker>
#include "Shell.h"
#include "ConnectionDefaults.h"
#include "DockManager.h"
#include "DockWidget.h"
#include "DockAreaWidget.h"
#include "DockContainerWidget.h"
#include "DockWidgetTab.h"
#include "qttreepropertybrowser_p.h"
#include <QActionGroup>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include "PreviewData.h"
#include <QPointer>
#include <QTreeWidgetItemIterator>
#include <functional>

using namespace preview;

PreviewWindow::PreviewWindow(QWidget* parent)
    : QMainWindow(parent), ui(std::make_unique<ShellUi>()) {
    Branding::initialize();
    static const bool resources = [] { Q_INIT_RESOURCE(preview_icons); return true; }();
    Q_UNUSED(resources);
    ui->setupUi(this);
    setWindowIcon(Branding::applicationIcon());
#ifndef Q_OS_MACOS
    ui->menubar->setNativeMenuBar(false);
#endif
    // uic installs the Designer toolbar child through QWidgetAction.
    for (auto* action : {ui->actionUnlockPasswords, ui->actionLockPasswords, ui->actionChangeMaster, ui->actionDifferentCredentials}) {
        action->setEnabled(false); action->setVisible(false);
    }
    sizeQuickConnect(ui->quickConnectControls, ui->quickConnectCombo, ui->quickConnectToolbar);
    ui->quickConnectCombo->lineEdit()->setPlaceholderText("Hostname or IP (preview only)");
    ui->menuConnectionPanels->setIcon(icon("Panel_16x.png"));
    ui->savedConnectionsButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    auto* protocols = new QMenu(ui->quickConnectButton);
    auto* protocolActions = new QActionGroup(protocols);
    // Match the supplied project's ProtocolType enum, including local-only protocols.
    for (const auto& name : {"RDP", "VNC", "SSH1", "SSH2", "Telnet", "Rlogin", "RAW",
                             "HTTP", "HTTPS", "PowerShell", "ARD", "Terminal", "WSL", "AnyDesk", "IntApp"}) {
        auto* action = protocols->addAction(name);
        action->setCheckable(true);
        action->setChecked(protocol == name);
        protocolActions->addAction(action);
        connect(action, &QAction::triggered, this, [this, name] {
            protocol = QString::fromLatin1(name);
            ui->quickConnectButton->setText(protocol);
        });
    }
    ui->quickConnectButton->setMenu(protocols);
    auto quickConnect = [this] {
        const auto title = ui->quickConnectCombo->currentText().trimmed();
        if (title.isEmpty()) { ui->quickConnectCombo->setFocus(); return; }
        openPreview(title, protocol);
        if (ui->quickConnectCombo->findText(title) < 0) ui->quickConnectCombo->addItem(title);
        ui->quickConnectCombo->setCurrentText(title);
    };
    connect(ui->quickConnectButton, &QToolButton::clicked, this, quickConnect);
    connect(ui->quickConnectCombo->lineEdit(), &QLineEdit::returnPressed, this, quickConnect);
    auto* saved = new QMenu(ui->savedConnectionsButton);
    ui->savedConnectionsButton->setMenu(saved);
    connect(saved, &QMenu::aboutToShow, this, [this, saved] {
        saved->clear();
        for (QTreeWidgetItemIterator it(ui->connectionTree); *it; ++it) {
            auto* node = *it;
            if (node->data(0, KindRole).toString() != "connection") continue;
            const auto name = node->text(0);
            const auto endpoint = node->data(0, EndpointRole).toString();
            const auto selectedProtocol = node->data(0, ProtocolRole).toString();
            connect(saved->addAction(node->icon(0), name), &QAction::triggered,
                    this, [this, name, endpoint, selectedProtocol] {
                openPreview(endpoint, selectedProtocol, name);
            });
        }
    });

    ui->multiSshToolbar->addWidget(new QLabel("Multi SSH:", ui->multiSshToolbar));
    auto* command = new QLineEdit(ui->multiSshToolbar);
    command->setObjectName("multiSshCommand");
    command->setFixedWidth(300);
    command->setReadOnly(true);
    command->setPlaceholderText("Preview only — command broadcast disabled");
    ui->multiSshToolbar->addWidget(command);

    for (auto* toolbar : {ui->connectionsToolbar, ui->configToolbar}) {
        toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
        for (auto* action : toolbar->actions()) {
            if (!action->isSeparator()) {
                if (action->toolTip().isEmpty()) action->setToolTip(action->text());
            }
        }
    }
    ui->searchEdit->addAction(icon("Search_16x.png"), QLineEdit::LeadingPosition)->setEnabled(false);

    const auto assets = qEnvironmentVariable("RM_PREVIEW_ASSET_DIR");
    if (!assets.isEmpty()) {
        const QIcon reconnect(assets + "/reconnect.png");
        if (!reconnect.isNull()) ui->actionTabReconnect->setIcon(reconnect);
        const QIcon external(assets + "/external-tools.png");
        if (!external.isNull()) ui->actionTabExternalTools->setIcon(external);
    }

    auto bindDock = [this](ads::CDockWidget* dock, QAction* action) {
        connect(action, &QAction::toggled, dock, [dock](bool visible) { if (visible == dock->isClosed()) dock->toggleView(visible); });
        connect(dock, &ads::CDockWidget::viewToggled, action, [action](bool open) {
            const QSignalBlocker blocker(action); action->setChecked(open);
        });
    };
    bindDock(ui->connectionsDock, ui->actionViewConnections);
    bindDock(ui->configDock, ui->actionViewConfig);
    bindDock(ui->notificationsDock, ui->actionViewNotifications);
    auto bindToolbar = [this](QToolBar* toolbar, QAction* action) {
        connect(action, &QAction::toggled, toolbar, &QWidget::setVisible);
        connect(toolbar, &QToolBar::visibilityChanged, action, [toolbar, action] {
            const QSignalBlocker blocker(action); action->setChecked(!toolbar->isHidden());
        });
    };
    bindToolbar(ui->quickConnectToolbar, ui->actionViewQuickConnect);
    bindToolbar(ui->multiSshToolbar, ui->actionViewMultiSsh);
    bindToolbar(ui->externalToolsToolbar, ui->actionViewExternalTools);
    connect(ui->actionResetLayout, &QAction::triggered, this, &PreviewWindow::resetLayout);
    connect(ui->actionLockToolbars, &QAction::toggled, this, [this](bool locked) {
        for (auto* bar : {ui->quickConnectToolbar, ui->multiSshToolbar, ui->externalToolsToolbar})
            bar->setMovable(!locked);
    });
    connect(ui->actionFullscreen, &QAction::toggled, this, [this](bool full) {
        if (full) showFullScreen(); else showNormal();
    });
    connect(ui->actionExit, &QAction::triggered, this, &QWidget::close);
    connect(ui->actionAbout, &QAction::triggered, this, [this] {
        auto* dialog = new QMessageBox(QMessageBox::NoIcon, "About CMRemote UI preview", "CMRemote — Cross-platform Multi Remote\n"
            "https://github.com/Crayjones/CMRemote", QMessageBox::Ok, this);
        dialog->setObjectName("aboutDialog");
        dialog->setIconPixmap(Branding::applicationIcon().pixmap(QSize(64, 64), dialog->devicePixelRatioF()));
        dialog->setTextFormat(Qt::PlainText); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->open();
    });
    connect(ui->actionAddPanel, &QAction::triggered, this, &PreviewWindow::addPanel);
    for (int direction : {1, -1}) connect(direction > 0 ? ui->actionNextSession : ui->actionPreviousSession, &QAction::triggered, this, [this, direction] {
        const auto docks = panelDocks(currentPanel()); if (docks.isEmpty()) return;
        const int index = std::max<int>(0, docks.indexOf(currentPanel()->currentDockWidget()));
        docks.at((index + docks.size() + direction) % docks.size())->setAsCurrentTab();
    });
    connect(ui->menuConnectionPanels, &QMenu::aboutToShow, this, &PreviewWindow::rebuildPanelsMenu);
    connect(ui->dockManager, &ads::CDockManager::focusedDockWidgetChanged, this, [this](ads::CDockWidget*, ads::CDockWidget* now) {
        if (isSessionDock(now)) lastSession = now;
    });
    // Dragging tabs changes panels; keep General's placeholder and panel styling current.
    const auto relayout = [this] { QMetaObject::invokeMethod(this, [this] { updateSessionLayout(); }, Qt::QueuedConnection); };
    connect(ui->dockManager, &ads::CDockManager::dockAreaCreated, this, relayout);
    connect(ui->dockManager, &ads::CDockManager::dockWidgetRemoved, this, relayout);
    connect(ui->dockManager, &ads::CDockManager::floatingWidgetCreated, this, relayout);
    connect(ui->dockManager, &ads::CDockContainerWidget::dockAreasRemoved, this, relayout);
    // The sample session the preview opens with.
    createPreview(ui->generalPanel(), "server01", "RDP", "server01", false);

    auto* root = ui->connectionTree->topLevelItem(0);
    root->setData(0, KindRole, "root");
    root->setIcon(0, nodeIcon("root"));
    root->child(0)->setData(0, KindRole, "connection");
    root->child(0)->setData(0, FavoriteRole, true);
    root->child(0)->setData(0, EndpointRole, "server01");
    root->child(0)->setData(0, ProtocolRole, "RDP");
    root->child(0)->setIcon(0, nodeIcon("connection"));
    auto* putty = ui->connectionTree->topLevelItem(1);
    putty->setData(0, KindRole, "putty");
    putty->setIcon(0, nodeIcon("putty"));
    root->setExpanded(true);
    connect(ui->connectionTree, &QTreeWidget::currentItemChanged, this, [this] { showProperties(); });
    connect(ui->connectionTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* node) {
        if (node->data(0, KindRole).toString() == "connection")
            openPreview(node->data(0, EndpointRole).toString(),
                        node->data(0, ProtocolRole).toString(), node->text(0));
    });
    connect(ui->searchEdit, &QLineEdit::textChanged, this, [this] { filterConnections(); });
    connect(ui->actionFavorites, &QAction::toggled, this, [this] { filterConnections(); });
    connect(ui->actionAddConnection, &QAction::triggered, this, [this] { addNode("connection"); });
    connect(ui->actionAddFolder, &QAction::triggered, this, [this] { addNode("folder"); });
    connect(ui->actionAddRoot, &QAction::triggered, this, [this] { addNode("root"); });
    connect(ui->actionExpandAll, &QAction::triggered, ui->connectionTree, &QTreeWidget::expandAll);
    connect(ui->actionCollapseAll, &QAction::triggered, ui->connectionTree, &QTreeWidget::collapseAll);
    ui->actionSort->setText("Sort");
    auto* sort = new QMenu(ui->connectionsToolbar);
    for (auto order : {Qt::AscendingOrder, Qt::DescendingOrder}) {
        auto* source = order == Qt::AscendingOrder ? ui->actionSortAscending : ui->actionSortDescending;
        auto* action = sort->addAction(source->icon(), source->text());
        connect(action, &QAction::triggered, this, [this, order] { ui->connectionTree->sortItems(0, order); });
    }
    ui->actionSort->setMenu(sort);
    if (auto* button = qobject_cast<QToolButton*>(ui->connectionsToolbar->widgetForAction(ui->actionSort)))
        button->setPopupMode(QToolButton::InstantPopup);
    connect(ui->actionSort, &QAction::triggered, this, [this] {
        ui->connectionTree->sortItems(0, Qt::AscendingOrder);
    });
    auto* modes = new QActionGroup(this);
    int mode = 0;
    for (auto* action : {ui->actionProperties, ui->actionInheritance,
                         ui->actionDefaultProperties, ui->actionDefaultInheritance}) {
        modes->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] { propertyMode = mode; showProperties(); });
        ++mode;
    }
    auto* sorting = new QActionGroup(this);
    for (auto* action : {ui->actionCategorized, ui->actionAlphabetical}) {
        sorting->addAction(action);
        connect(action, &QAction::triggered, this, [this] { showProperties(); });
    }
    documentEditor = new ProfileEditor(ui->configGrid);
    connect(documentEditor, &ProfileEditor::helpChanged, this, [this](const QString& title, const QString& text) {
        ui->configHelpTitle->setText(title);
        ui->configHelpText->setText(text);
    });
    // Edits stay in this grid. Applying the overrides locally lets Protocol show
    // and hide its settings the way the application's model does.
    connect(documentEditor, &ProfileEditor::changed, this, [this] {
        if (documentEditor->documentRoot() || documentEditor->defaultsMode() || documentEditor->id().isEmpty()) return;
        auto effective = previewEffective; const auto overrides = documentEditor->draft().value("overrides").toObject();
        for (auto it = overrides.begin(); it != overrides.end(); ++it) if (!it.value().isNull()) effective[it.key()] = it.value();
        documentEditor->preview(effective);
    });
    ui->connectionTree->setCurrentItem(root);
    ui->notificationSplitter->setSizes({226, 899});
    ui->configSplitter->setSizes({140, 65});
    connect(ui->notificationsList, &QListWidget::itemSelectionChanged,
            this, &PreviewWindow::updateNotificationDetails);
    ui->notificationsList->item(0)->setIcon(icon("StatusInformation_16x.png"));
    ui->notificationsList->setCurrentRow(0);
    connect(ui->actionShowHelp, &QAction::toggled, ui->configHelp, &QWidget::setVisible);
    connect(ui->actionShowToolText, &QAction::toggled, this, [this](bool text) {
        ui->externalToolsToolbar->setToolButtonStyle(text ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    });
    ui->externalToolsToolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    for (auto* tree : QList<QTreeWidget*>{ui->connectionTree, documentEditor->view()}) {
        tree->installEventFilter(this);
        tree->viewport()->installEventFilter(this);
    }
    connect(ui->menuJumpTo, &QMenu::aboutToShow, this, &PreviewWindow::rebuildJumpMenu);
    ui->menuView->setToolTipsVisible(true); rebuildJumpMenu();
    ui->notificationsList->installEventFilter(this);
    ui->notificationsList->viewport()->installEventFilter(this);
    ui->externalToolsToolbar->installEventFilter(this);
    resetLayout();
}

PreviewWindow::~PreviewWindow() {
    // The grid reports help changes while it tears down, after the forms are gone.
    if (documentEditor) documentEditor->disconnect(this);
}

void PreviewWindow::resetLayout() {
    ui->resetDocks();
    for (auto* toolbar : {ui->quickConnectToolbar, ui->multiSshToolbar, ui->externalToolsToolbar}) {
        removeToolBar(toolbar);
        addToolBar(Qt::TopToolBarArea, toolbar);
        toolbar->setMovable(!ui->actionLockToolbars->isChecked());
    }
    ui->quickConnectToolbar->show();
    ui->multiSshToolbar->hide();
    ui->externalToolsToolbar->hide();
}

QList<ads::CDockWidget*> PreviewWindow::panelDocks(ads::CDockAreaWidget* panel) const {
    QList<ads::CDockWidget*> result;
    if (panel) for (auto* dock : panel->dockWidgets()) if (isSessionDock(dock)) result.append(dock);
    return result;
}
QList<ads::CDockAreaWidget*> PreviewWindow::sessionPanels() const {
    QList<ads::CDockAreaWidget*> result;
    auto collect = [&](ads::CDockContainerWidget* container) {
        for (int i = 0; i < container->dockAreaCount(); ++i) if (!panelDocks(container->dockArea(i)).isEmpty()) result.append(container->dockArea(i));
    };
    collect(ui->dockManager);
    for (auto* floating : ui->dockManager->floatingWidgets()) collect(floating->dockContainer());
    if (result.removeAll(ui->generalPanel())) result.prepend(ui->generalPanel());
    return result;
}
QList<ads::CDockWidget*> PreviewWindow::sessionDocks() const {
    QList<ads::CDockWidget*> result;
    for (auto* panel : sessionPanels()) result += panelDocks(panel);
    return result;
}
QString PreviewWindow::panelName(ads::CDockAreaWidget* panel) const {
    if (panel == ui->generalPanel()) return "General";
    if (panel->property("panelName").toString().isEmpty()) panel->setProperty("panelName", QString("Panel %1").arg(nextPanel++));
    return panel->property("panelName").toString();
}
ads::CDockAreaWidget* PreviewWindow::currentPanel() const {
    if (lastSession && isSessionDock(lastSession)) if (auto* area = lastSession->dockAreaWidget()) return area;
    return ui->generalPanel();
}

void PreviewWindow::openPreview(const QString& endpoint, const QString& selectedProtocol, const QString& title) {
    for (auto* dock : sessionDocks()) {
        if (dock->widget()->property("previewTitle").toString() == endpoint &&
            dock->widget()->property("previewProtocol").toString() == selectedProtocol) {
            dock->setAsCurrentTab(); lastSession = dock;
            return;
        }
    }
    createPreview(currentPanel(), endpoint, selectedProtocol, title.isEmpty() ? endpoint : title);
}

void PreviewWindow::createPreview(ads::CDockAreaWidget* panel, const QString& endpoint,
                                  const QString& selectedProtocol, const QString& title, bool announce) {
    if (!panel) panel = ui->generalPanel();
    auto* page = new QWidget;
    // The reuse key is independent of the mutable tab caption.
    page->setProperty("previewTitle", endpoint);
    page->setProperty("previewProtocol", selectedProtocol);
    page->setObjectName("sessionSurface"); page->setWindowTitle(title);
    auto* layout = new QVBoxLayout(page);
    auto* text = new QLabel(endpoint + " · " + selectedProtocol + "\n\nUI preview only — no network connection", page);
    text->setObjectName("sessionMessage");
    text->setTextFormat(Qt::PlainText);
    text->setAlignment(Qt::AlignCenter);
    text->setWordWrap(true);
    layout->addWidget(text);
    auto* dock = createSessionDock(ui->dockManager, page, QString("previewSession%1").arg(nextSession++));
    dock->setIcon(nodeIcon("connection"));
    dock->tabWidget()->installEventFilter(this);
    connect(dock, &ads::CDockWidget::closeRequested, this, [this, target = QPointer<ads::CDockWidget>(dock)] {
        if (target) closeSession(target);
    }, Qt::QueuedConnection);
    panel->dockManager()->addDockWidgetTabToArea(dock, panel);
    dock->setAsCurrentTab(); lastSession = dock;
    updateSessionLayout();
    if (!announce) return;
    new QListWidgetItem(icon("StatusInformation_16x.png"),
                        "Opened local preview tab: " + title + " (" + selectedProtocol + ")", ui->notificationsList);
    ui->notificationsList->setCurrentRow(ui->notificationsList->count() - 1, QItemSelectionModel::ClearAndSelect);
}

void PreviewWindow::closeSession(ads::CDockWidget* dock) {
    if (!isSessionDock(dock)) return;
    if (auto* manager = dock->dockManager()) manager->removeDockWidget(dock);
    dock->deleteLater();
    updateSessionLayout();
}

void PreviewWindow::updateSessionLayout() {
    // As in the application: General shows its placeholder only while no session is docked in the window.
    bool sessions = false;
    for (auto* panel : sessionPanels()) sessions = sessions || !panel->dockContainer()->isFloating();
    markSessionPanels(ui->dockManager);
    ui->generalPanel()->setDockAreaFlag(ads::CDockAreaWidget::HideSingleWidgetTitleBar, !sessions);
    showGeneralPlaceholder(ui->documentsDock, !sessions);
}

void PreviewWindow::updateNotificationDetails() {
    const auto selected = ui->notificationsList->selectedItems();
    ui->notificationText->setPlainText(selected.size() == 1 ? selected.first()->text() : QString());
    ui->notificationDate->setText(selected.size() == 1 ? "UI preview" : "");
    ui->notificationIcon->setVisible(selected.size() == 1);
}

void PreviewWindow::addPanel() {
    // New Panel moves the current tab into a panel of its own beside its panel.
    auto* panel = currentPanel(); const auto docks = panelDocks(panel);
    if (docks.size() < 2 || !isSessionDock(panel->currentDockWidget())) return;
    auto* dock = panel->currentDockWidget();
    splitPanel(ui->dockManager, dock, panel, ads::RightDockWidgetArea);
    lastSession = dock; updateSessionLayout();
}

void PreviewWindow::rebuildPanelsMenu() {
    auto* menu = ui->menuConnectionPanels; menu->clear();
    for (auto* panel : sessionPanels()) {
        auto* action = menu->addAction(icon("Panel_16x.png"), panelName(panel));
        connect(action, &QAction::triggered, this, [panel = QPointer<ads::CDockAreaWidget>(panel)] {
            if (panel && panel->currentDockWidget()) panel->currentDockWidget()->setAsCurrentTab();
        });
    }
    if (menu->isEmpty()) menu->addAction("No open panels")->setEnabled(false);
}

void PreviewWindow::addNode(const QString& kind) {
    auto* parent = ui->connectionTree->currentItem();
    if (parent && parent->data(0, KindRole).toString() == "connection") parent = parent->parent();
    if (kind == "root") parent = nullptr;
    if (parent && parent->data(0, KindRole).toString() == "putty") return;
    auto* node = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(ui->connectionTree);
    node->setText(0, QString("New %1 %2").arg(kind).arg(nextNode++));
    node->setData(0, KindRole, kind);
    if (kind == "connection") {
        node->setData(0, EndpointRole, QString("preview-host-%1").arg(nextNode - 1));
        node->setData(0, ProtocolRole, "RDP");
    }
    node->setIcon(0, nodeIcon(kind));
    if (parent) parent->setExpanded(true);
    ui->connectionTree->setCurrentItem(node);
    filterConnections();
}

void PreviewWindow::filterConnections() {
    const auto search = ui->searchEdit->text().trimmed();
    std::function<bool(QTreeWidgetItem*)> filter = [&](QTreeWidgetItem* node) {
        bool childMatch = false;
        for (int i = 0; i < node->childCount(); ++i) childMatch = filter(node->child(i)) || childMatch;
        bool match = node->text(0).contains(search, Qt::CaseInsensitive);
        if (ui->actionFavorites->isChecked()) match = match && node->data(0, FavoriteRole).toBool();
        node->setHidden(!(match || childMatch));
        if (!search.isEmpty() && childMatch) node->setExpanded(true);
        return match || childMatch;
    };
    for (int i = 0; i < ui->connectionTree->topLevelItemCount(); ++i) filter(ui->connectionTree->topLevelItem(i));
}

void PreviewWindow::showProperties() {
    auto* node = ui->connectionTree->currentItem();
    if (!node) { documentEditor->load({}, {}); return; }
    const auto kind = node->data(0, KindRole).toString();
    ui->actionConnectionIcon->setIcon(nodeIcon(kind));
    const bool canInherit = node->parent() != nullptr;
    ui->actionInheritance->setEnabled(canInherit);
    if (!canInherit && propertyMode == 1) { propertyMode = 0; ui->actionProperties->setChecked(true); }
    ui->configDock->setWindowTitle(propertyMode == 0 ? "Config" :
                                 propertyMode == 1 ? "Config — Inheritance" :
                                 propertyMode == 2 ? "Config — Default properties" : "Config — Default inheritance");
    if (kind == "root" && propertyMode == 0) {
        documentEditor->loadDocument(false, false, "Preview only: display-only values. No connection file or master-password workflow is accessible.", false);
        documentEditor->setCategorized(ui->actionCategorized->isChecked());
        documentEditor->setInheritance(false);
        return;
    }
    documentEditor->setCategorized(ui->actionCategorized->isChecked());
    if (propertyMode >= 2) { documentEditor->loadDefaults({}, propertyMode == 3); return; }
    // In-memory sample settings: the Settings defaults with this node's endpoint.
    previewEffective = ConnectionDefaults{}.effective();
    const auto protocol = node->data(0, ProtocolRole).toString().toLower();
    if (QStringList{"rdp", "ssh", "vnc"}.contains(protocol)) {
        previewEffective["protocol"] = protocol; previewEffective["port"] = protocol == "ssh" ? 22 : protocol == "vnc" ? 5900 : 3389;
    }
    const auto endpoint = node->data(0, EndpointRole).toString();
    if (kind == "connection") previewEffective["host"] = endpoint;
    documentEditor->load(QJsonObject{{"id", "preview-" + node->text(0)}, {"kind", kind == "folder" ? "folder" : "connection"},
        {"name", node->text(0)}, {"parent", node->parent() && node->parent()->parent() ? QJsonValue("preview-folder") : QJsonValue(QJsonValue::Null)},
        {"overrides", kind == "connection" ? QJsonObject{{"host", endpoint}} : QJsonObject{}}, {"effective", previewEffective}}, {});
    documentEditor->setInheritance(propertyMode == 1);
}
