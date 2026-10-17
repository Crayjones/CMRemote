#include "PreviewWindow.h"
#include "Branding.h"
#include "ProfileEditor.h"
#include "QuickConnect.h"
#include <QSignalBlocker>
#include "ui_test.h"
#include <QActionGroup>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include "PreviewData.h"
#include <QPointer>
#include <QTabBar>
#include <QTreeWidgetItemIterator>
#include <functional>

using namespace preview;

PreviewWindow::PreviewWindow(QWidget* parent)
    : QMainWindow(parent), ui(std::make_unique<Ui::MainWindow>()) {
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
    ui->connectionsDock->setWindowIcon(icon("ASPWebSite_16x.png"));
    ui->configDock->setWindowIcon(icon("Settings_16x.png"));
    ui->notificationsDock->setWindowIcon(icon("StatusInformation_16x.png"));

    const auto assets = qEnvironmentVariable("RM_PREVIEW_ASSET_DIR");
    if (!assets.isEmpty()) {
        const QIcon reconnect(assets + "/reconnect.png");
        if (!reconnect.isNull()) ui->actionTabReconnect->setIcon(reconnect);
        const QIcon external(assets + "/external-tools.png");
        if (!external.isNull()) ui->actionTabExternalTools->setIcon(external);
    }

    auto bindDock = [this](QDockWidget* dock, QAction* action) {
        connect(action, &QAction::toggled, dock, &QWidget::setVisible);
        connect(dock, &QDockWidget::visibilityChanged, action, [dock, action] {
            const QSignalBlocker blocker(action); action->setChecked(!dock->isHidden());
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
        auto* dialog = new QMessageBox(QMessageBox::NoIcon, "About CMRemote UI preview", "CMRemote shell preview.\n"
            "No networking, credentials, profile files or backend changes.", QMessageBox::Ok, this);
        dialog->setObjectName("aboutDialog");
        dialog->setIconPixmap(Branding::applicationIcon().pixmap(QSize(64, 64), dialog->devicePixelRatioF()));
        dialog->setTextFormat(Qt::PlainText); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->open();
    });
    connect(ui->actionAddPanel, &QAction::triggered, this, &PreviewWindow::addPanel);
    connect(ui->actionGeneralPanel, &QAction::triggered, this, [this] {
        ui->groupTabs->setCurrentWidget(ui->generalPanel);
    });
    connect(ui->actionNextSession, &QAction::triggered, this, [this] {
        auto* tabs = currentSessions();
        if (tabs && tabs->count()) tabs->setCurrentIndex((tabs->currentIndex() + 1) % tabs->count());
    });
    connect(ui->actionPreviousSession, &QAction::triggered, this, [this] {
        auto* tabs = currentSessions();
        if (tabs && tabs->count()) tabs->setCurrentIndex((tabs->currentIndex() + tabs->count() - 1) % tabs->count());
    });
    configureTabs(ui->sessionTabs);
    ui->serverTab->setProperty("previewTitle", "server01");
    ui->serverTab->setProperty("previewProtocol", "RDP");
    ui->sessionTabs->setTabIcon(0, nodeIcon("connection"));
    ui->groupTabs->setTabIcon(0, Branding::applicationIcon());
    ui->generalPanel->setProperty("isGeneralPanel", true);
    ui->generalPanel->setProperty("panelAction", QVariant::fromValue(ui->actionGeneralPanel));
    ui->groupTabs->tabBar()->installEventFilter(this);

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
    ui->configTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    ui->configTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->configTree->setColumnWidth(0, 115);
    connect(ui->configTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* row) {
        if (documentEditor && documentEditor->documentRoot()) return;
        ui->configHelpTitle->setText(row ? row->text(0) : QString());
        ui->configHelpText->setText(row ? row->data(0, Qt::UserRole).toString() : QString());
    });
    documentEditor = new ProfileEditor(ui->configTree);
    connect(documentEditor, &ProfileEditor::helpChanged, this, [this](const QString& title, const QString& text) {
        if (!documentEditor->documentRoot()) return;
        ui->configHelpTitle->setText(title);
        ui->configHelpText->setText(text);
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
    for (auto* tree : QList<QTreeWidget*>{ui->connectionTree, ui->configTree}) {
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

PreviewWindow::~PreviewWindow() = default;

void PreviewWindow::resetLayout() {
    setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
    for (auto* dock : {ui->connectionsDock, ui->configDock, ui->notificationsDock}) {
        dock->setFloating(false);
        removeDockWidget(dock);
    }
    addDockWidget(Qt::LeftDockWidgetArea, ui->connectionsDock);
    splitDockWidget(ui->connectionsDock, ui->configDock, Qt::Vertical);
    addDockWidget(Qt::BottomDockWidgetArea, ui->notificationsDock);
    ui->connectionsDock->show(); ui->configDock->show(); ui->notificationsDock->hide();
    resizeDocks({ui->connectionsDock}, {226}, Qt::Horizontal);
    resizeDocks({ui->connectionsDock, ui->configDock}, {185, 215}, Qt::Vertical);
    resizeDocks({ui->notificationsDock}, {105}, Qt::Vertical);
    for (auto* toolbar : {ui->quickConnectToolbar, ui->multiSshToolbar, ui->externalToolsToolbar}) {
        removeToolBar(toolbar);
        addToolBar(Qt::TopToolBarArea, toolbar);
        toolbar->setMovable(!ui->actionLockToolbars->isChecked());
    }
    ui->quickConnectToolbar->show();
    ui->multiSshToolbar->hide();
    ui->externalToolsToolbar->hide();
}

QTabWidget* PreviewWindow::currentSessions() const {
    auto* panel = ui->groupTabs->currentWidget();
    return panel ? panel->findChild<QTabWidget*>() : nullptr;
}

void PreviewWindow::openPreview(const QString& endpoint, const QString& selectedProtocol, const QString& title) {
    auto* tabs = currentSessions();
    if (!tabs) return;
    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->widget(i)->property("previewTitle").toString() == endpoint &&
            tabs->widget(i)->property("previewProtocol").toString() == selectedProtocol) {
            tabs->setCurrentIndex(i);
            return;
        }
    }
    createPreview(tabs, endpoint, selectedProtocol, title.isEmpty() ? endpoint : title);
}

void PreviewWindow::createPreview(QTabWidget* tabs, const QString& endpoint,
                                  const QString& selectedProtocol, const QString& title) {
    auto* page = new QWidget(tabs);
    // The reuse key is independent of the mutable tab caption.
    page->setProperty("previewTitle", endpoint);
    page->setProperty("previewProtocol", selectedProtocol);
    page->setStyleSheet("background: #191919; color: #dddddd;");
    auto* layout = new QVBoxLayout(page);
    auto* text = new QLabel(endpoint + " · " + selectedProtocol + "\n\nUI preview only — no network connection", page);
    text->setTextFormat(Qt::PlainText);
    text->setAlignment(Qt::AlignCenter);
    text->setWordWrap(true);
    layout->addWidget(text);
    tabs->setCurrentIndex(tabs->addTab(page, nodeIcon("connection"), title));
    new QListWidgetItem(icon("StatusInformation_16x.png"),
                        "Opened local preview tab: " + title + " (" + selectedProtocol + ")", ui->notificationsList);
    ui->notificationsList->setCurrentRow(ui->notificationsList->count() - 1, QItemSelectionModel::ClearAndSelect);
}

void PreviewWindow::configureTabs(QTabWidget* tabs) {
    tabs->setDocumentMode(true);
    tabs->setTabsClosable(true);
    tabs->setMovable(true);
    tabs->tabBar()->installEventFilter(this);
    connect(tabs, &QTabWidget::tabCloseRequested, this, [this, tabs](int index) {
        closeSession(tabs, tabs->widget(index));
    });
}

void PreviewWindow::closeSession(QTabWidget* tabs, QWidget* page) {
    const int index = tabs->indexOf(page);
    if (index < 0) return;
    tabs->removeTab(index);
    page->deleteLater();
}

void PreviewWindow::updateNotificationDetails() {
    const auto selected = ui->notificationsList->selectedItems();
    ui->notificationText->setPlainText(selected.size() == 1 ? selected.first()->text() : QString());
    ui->notificationDate->setText(selected.size() == 1 ? "UI preview" : "");
    ui->notificationIcon->setVisible(selected.size() == 1);
}

void PreviewWindow::addPanel() {
    const auto name = QString("Panel %1").arg(nextPanel++);
    auto* panel = new QWidget(ui->groupTabs);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* tabs = new QTabWidget(panel);
    tabs->setObjectName("panelSessionTabs");
    configureTabs(tabs);
    layout->addWidget(tabs);
    ui->groupTabs->setCurrentIndex(ui->groupTabs->addTab(panel, Branding::applicationIcon(), name));
    auto* action = ui->menuConnectionPanels->addAction(name);
    panel->setProperty("panelAction", QVariant::fromValue(action));
    connect(action, &QAction::triggered, this, [this, target = QPointer<QWidget>(panel)] {
        if (target && ui->groupTabs->indexOf(target) >= 0) ui->groupTabs->setCurrentWidget(target);
    });
    connect(panel, &QObject::destroyed, action, &QObject::deleteLater);
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
    documentEditor->load({}, {});
    ui->configTree->clear();
    ui->configTree->setEnabled(true);
    QTreeWidgetItem* first = nullptr;
    auto property = [&](const QString& category, const QString& name, const QString& value, const QString& help) {
        QTreeWidgetItem* parent = nullptr;
        if (ui->actionCategorized->isChecked()) {
            for (int i = 0; i < ui->configTree->topLevelItemCount(); ++i)
                if (ui->configTree->topLevelItem(i)->text(0) == category) parent = ui->configTree->topLevelItem(i);
            if (!parent) {
                parent = new QTreeWidgetItem(ui->configTree, {category});
                parent->setBackground(0, QColor("#eeeeee"));
                parent->setBackground(1, QColor("#eeeeee"));
                auto font = parent->font(0); font.setBold(true); parent->setFont(0, font);
                parent->setExpanded(true);
            }
        }
        auto* row = parent ? new QTreeWidgetItem(parent, {name, value}) : new QTreeWidgetItem(ui->configTree, {name, value});
        row->setData(0, Qt::UserRole, help);
        row->setToolTip(1, "Read-only preview value; no profile is modified");
        if (!first) first = row;
    };
    if (propertyMode == 1 || propertyMode == 3) {
        property("Inheritance", "Protocol", "Yes", "Preview inheritance flag. The Rust model is not connected.");
        property("Inheritance", "Hostname / IP", "No", "Preview local-host override flag.");
        property("Inheritance", "Port", "Yes", "Preview inheritance flag for the protocol port.");
    } else {
        const auto name = propertyMode == 2 ? QString("Defaults") : node->text(0);
        property("Display", "Name", name, "This is the name displayed in the connections tree.");
        if (kind != "putty" || propertyMode == 2) {
            property("Display", "Description", "", "A description for the selected connection or folder.");
            property("Connection", "Protocol", "RDP", "The protocol used by a connection. Preview only.");
            if (kind == "connection") property("Connection", "Hostname / IP", node->data(0, EndpointRole).toString(), "Remote endpoint; no connection is made by this preview.");
            property("Connection", "Port", "3389", "Default RDP port, shown as a read-only preview value.");
            property("Connection", "Username", "", "Read-only account preview; no credentials are requested.");
            property("Connection", "Password", "Unavailable", "Preview cannot accept or store passwords.");
            property("Connection", "Domain", "", "Read-only account domain preview.");
        }
    }
    if (ui->actionAlphabetical->isChecked()) ui->configTree->sortItems(0, Qt::AscendingOrder);
    ui->configTree->setCurrentItem(first);
}
