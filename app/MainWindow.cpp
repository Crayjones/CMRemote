#include "MainWindow.h"
#include "Branding.h"
#include <functional>
#include "Credentials.h"
#include "ProfileEditor.h"
#include "ConnectionIcons.h"
#include "SessionPane.h"
#include "RdpWorker.h"
#include "SshWorker.h"
#include "VncWorker.h"
#include "SshHostKeyService.h"
#include "TerminalAppearanceService.h"
#include "TerminalAppearanceDialog.h"
#include "DataPaths.h"
#include "QuickConnect.h"
#include "OptionsDialog.h"
#include "OpenH264.h"
#include "LocalShortcuts.h"
#include "Views.h"
#include "SessionFullscreenController.h"
#include "PanelWindow.h"
#include "ShellVisibility.h"
#include "UpdateCheck.h"
#include "AutoUpdate.h"
#include "ui_test.h"
#include <QDir>
#include <QActionGroup>
#include <QScrollBar>
#include <QCloseEvent>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPushButton>
#include <QMouseEvent>
#include <QToolBar>
#include <QWindow>
#include <QGuiApplication>
#include <QMenu>
#include <QPointer>
#include <QTimer>
#include <QStatusBar>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <exception>

static QString qstr(const rust::String& s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }
static std::string bytes(const QString& s) { return s.toUtf8().toStdString(); }
static QIcon originalIcon(const QString& name) { return QIcon(":/original/" + name); }
#ifdef Q_OS_MACOS
QSize rmNativeTitleBarInset(QWidget* widget);
#endif

MainWindow::MainWindow(const QString& dataPath, bool testMode, QWidget* parent, std::shared_ptr<ClipboardEndpoint> clipboard, QString sshHostKeyPath, QString terminalAppearancePath, const QString& preferencesPath)
    : QMainWindow(parent), sshHostKeyPath_(std::move(sshHostKeyPath)), ui_(std::make_unique<Ui::MainWindow>()), core_(rm::detached_core()), dataPath_(dataPath), testMode_(testMode) {
    Branding::initialize();
    clipboardEndpoint_ = clipboard ? std::move(clipboard) : systemClipboardEndpoint();
    static const bool resources = [] { Q_INIT_RESOURCE(preview_icons); return true; }();
    Q_UNUSED(resources);
    ui_->setupUi(this);
#ifndef Q_OS_MACOS
    ui_->menubar->setNativeMenuBar(false);
#endif
    setWindowTitle("CMRemote");
    // No bottom status strip; messages go to Notifications.
    statusBar()->hide();
    setWindowIcon(Branding::applicationIcon());
    ui_->groupTabs->setTabIcon(ui_->groupTabs->indexOf(ui_->generalPanel), Branding::applicationIcon());
    while (ui_->sessionTabs->count()) { auto* page = ui_->sessionTabs->widget(0); ui_->sessionTabs->removeTab(0); delete page; }
    ui_->serverTab = nullptr; ui_->notificationsList->clear(); ui_->notificationText->clear(); ui_->notificationDate->clear();
    ui_->generalPanel->setProperty("isGeneralPanel", true);
    ui_->connectionTree->setSelectionMode(QAbstractItemView::SingleSelection);
    editor_ = new ProfileEditor(ui_->configTree);
    for (auto* action : findChildren<QAction*>()) {
        if (!action->objectName().startsWith("action")) continue;
        action->setEnabled(false); action->setToolTip("Unavailable in this milestone");
    }
    for (auto* action : {ui_->actionAddFolder, ui_->actionAddConnection, ui_->actionTreeConnect, ui_->actionTreeDelete,
         ui_->actionTreeRename, ui_->actionTabDisconnect, ui_->actionTabReconnect, ui_->actionExit, ui_->actionAbout,
         ui_->actionViewConnections, ui_->actionViewConfig, ui_->actionViewNotifications, ui_->actionViewQuickConnect, ui_->actionViewPanels,
         ui_->actionViewExternalTools, ui_->actionViewMultiSsh, ui_->actionGeneralPanel, ui_->actionExpandAll,
         ui_->actionCollapseAll, ui_->actionSort, ui_->actionShowHelp, ui_->actionCategorized, ui_->actionAlphabetical}) {
        action->setEnabled(true); action->setToolTip(action->text());
    }
    if (!terminalAppearancePath.isEmpty()) {
        terminalAppearance_ = new TerminalAppearanceService(std::move(terminalAppearancePath), this); appearanceStopped_ = false;
        connect(terminalAppearance_, &TerminalAppearanceService::applied, this, [this] {
            if (closeApproved_) return;
            for (auto* pane : sessionPages()) if (pane->target().protocol == "ssh")
                if (auto* view = pane->findChild<TerminalView*>()) view->setAppearance(terminalAppearance_->current());
        });
        connect(terminalAppearance_, &TerminalAppearanceService::changed, this, [this] {
            if (!closeApproved_ && terminalAppearance_->ready() && !terminalAppearance_->error().isEmpty())
                notification("Terminal appearance: " + terminalAppearance_->error(), Notice::Warning);
        });
    }
    // Options has its own dialog (showOptions); only this action opens Terminal Appearance.
    ui_->actionTerminalAppearance->setEnabled(true); ui_->actionTerminalAppearance->setToolTip("Font, colors and cursor for all SSH terminals");
    connect(ui_->actionTerminalAppearance, &QAction::triggered, this, &MainWindow::showTerminalAppearance);
    ui_->actionAbout->setText("About CMRemote");
    ui_->actionTabDisconnect->setToolTip("Disconnect; keep this tab until Close");
    sizeQuickConnect(ui_->quickConnectControls, ui_->quickConnectCombo, ui_->quickConnectToolbar);
    ui_->quickConnectCombo->lineEdit()->setPlaceholderText("Hostname or IP");
    ui_->quickConnectCombo->setToolTip("Temporary target / address history; no profile is created");
    ui_->quickConnectButton->setToolTip("Open a temporary session; the arrow chooses the protocol");
    // Only protocols with a working backend are listed.
    auto* protocols = new QMenu(ui_->quickConnectButton); protocols->setObjectName("quickConnectProtocols"); protocols->setToolTipsVisible(true);
    auto* choices = new QActionGroup(protocols);
    for (const auto& [name, protocol] : std::initializer_list<std::pair<QString, QString>>{{"RDP", "rdp"}, {"VNC", "vnc"}, {"SSH", "ssh"}}) {
        auto* action = protocols->addAction(name); action->setData(protocol); action->setCheckable(true); choices->addAction(action);
        action->setToolTip("Quick Connect with " + name);
        connect(action, &QAction::triggered, this, [this, protocol] { setQuickConnectProtocol(protocol); });
    }
    ui_->quickConnectButton->setMenu(protocols); setQuickConnectProtocol("rdp");
    // The globe lists every open file's folders and connections; choosing one connects it.
    auto* saved = new QMenu(ui_->savedConnectionsButton); saved->setObjectName("savedConnectionsMenu");
    ui_->savedConnectionsButton->setMenu(saved); ui_->savedConnectionsButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    ui_->savedConnectionsButton->setToolTip("Connections");
    connect(saved, &QMenu::aboutToShow, this, [this, saved] { rebuildConnectionsMenu(saved); });
    connect(ui_->quickConnectButton, &QToolButton::clicked, this, [this] { quickConnect(ui_->quickConnectCombo->currentText()); });
    connect(ui_->quickConnectCombo->lineEdit(), &QLineEdit::returnPressed, this, [this] { quickConnect(ui_->quickConnectCombo->currentText()); });
    connect(ui_->actionSaveAsConnection, &QAction::triggered, this, &MainWindow::saveAsConnection);
    ui_->searchEdit->addAction(originalIcon("Search_16x.png"), QLineEdit::LeadingPosition)->setEnabled(false);
    ui_->connectionsDock->setWindowIcon(originalIcon("ASPWebSite_16x.png"));
    ui_->configDock->setWindowIcon(originalIcon("Settings_16x.png"));
    ui_->notificationsDock->setWindowIcon(originalIcon("StatusInformation_16x.png"));
    for (auto* toolbar : {ui_->connectionsToolbar, ui_->configToolbar}) toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    ui_->actionAddFolder->setText("New folder"); ui_->actionSort->setText("Sort");
    auto* sort = new QMenu(this); ui_->actionSort->setMenu(sort);
    for (auto order : {Qt::AscendingOrder, Qt::DescendingOrder}) menuAction(sort, order == Qt::AscendingOrder ? "actionSortAscending" : "actionSortDescending", true, [this, order] { sortConnections(order); });
    qobject_cast<QToolButton*>(ui_->connectionsToolbar->widgetForAction(ui_->actionSort))->setPopupMode(QToolButton::InstantPopup);
    connect(ui_->actionAddFolder, &QAction::triggered, this, &MainWindow::addFolder);
    connect(ui_->actionAddConnection, &QAction::triggered, this, &MainWindow::addConnection);
    connect(ui_->actionTreeRename, &QAction::triggered, this, [this] { editor_->focusName(); });
    connect(ui_->actionTreeConnect, &QAction::triggered, this, [this] { connectSelected(false); });
    connect(ui_->actionTabDisconnect, &QAction::triggered, this, &MainWindow::disconnectCurrent);
    connect(ui_->actionTabReconnect, &QAction::triggered, this, &MainWindow::reconnectCurrent);
    connect(ui_->actionTreeDelete, &QAction::triggered, this, &MainWindow::deleteSelected);
    setupTreeActions();
    connect(ui_->actionExit, &QAction::triggered, this, &QWidget::close);
    connect(ui_->actionAbout, &QAction::triggered, this, [this] {
        const QString repository = Updates::Repository;
        auto* dialog = new QMessageBox(QMessageBox::NoIcon, "About CMRemote", "CMRemote — Cross-platform Multi Remote<br>"
            "<a href=\"" + repository + "\">" + repository + "</a>", QMessageBox::Ok, this);
        dialog->setObjectName("aboutDialog");
        dialog->setIconPixmap(Branding::applicationIcon().pixmap(QSize(64, 64), dialog->devicePixelRatioF()));
        dialog->setTextFormat(Qt::RichText); dialog->setTextInteractionFlags(Qt::TextBrowserInteraction);
        if (auto* label = dialog->findChild<QLabel*>("qt_msgbox_label")) label->setOpenExternalLinks(true);
        dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->open();
    });
    ui_->actionCheckUpdates->setText("Check for Updates…"); ui_->actionCheckUpdates->setEnabled(true);
    ui_->actionCheckUpdates->setToolTip("Compare this version with the latest CMRemote release on GitHub");
    // macOS lists it in the application menu, under About.
    ui_->actionCheckUpdates->setMenuRole(QAction::ApplicationSpecificRole);
    connect(ui_->actionCheckUpdates, &QAction::triggered, this, &MainWindow::checkForUpdates);
    connect(editor_, &ProfileEditor::changed, this, &MainWindow::previewDraft);
    connect(editor_, &ProfileEditor::userCommitted, this, &MainWindow::queueAutoSave);
    connect(editor_, &ProfileEditor::modeChanged, this, &MainWindow::updateActions);
    connect(editor_, &ProfileEditor::helpChanged, this, [this](const QString& title, const QString& text) {
        if (!ui_->actionShowHelp->isChecked()) return;
        ui_->configHelpTitle->setText(title); ui_->configHelpText->setText(text);
    });
    connect(ui_->actionShowHelp, &QAction::toggled, this, [this](bool visible) { ui_->configHelp->setVisible(visible); if (visible) editor_->showHelp(); });
    connect(ui_->actionCategorized, &QAction::triggered, this, [this] { editor_->setCategorized(true); });
    connect(ui_->actionAlphabetical, &QAction::triggered, this, [this] { editor_->setCategorized(false); });
    ui_->actionCategorized->setEnabled(true); ui_->actionAlphabetical->setEnabled(true);
    ui_->actionCategorized->setToolTip(ui_->actionCategorized->text()); ui_->actionAlphabetical->setToolTip(ui_->actionAlphabetical->text());
    auto* order = new QActionGroup(this); order->addAction(ui_->actionCategorized); order->addAction(ui_->actionAlphabetical);
    auto* values = new QActionGroup(this);
    for (auto* action : {ui_->actionProperties, ui_->actionInheritance, ui_->actionDefaultProperties, ui_->actionDefaultInheritance}) {
        values->addAction(action); action->setCheckable(true); action->setToolTip(action->text());
    }
    ui_->actionProperties->setEnabled(true); ui_->actionInheritance->setEnabled(true);
    connect(ui_->actionProperties, &QAction::triggered, this, [this] { setConfigMode(false, false); });
    connect(ui_->actionInheritance, &QAction::triggered, this, [this] { setConfigMode(false, true); });
    connect(ui_->actionDefaultProperties, &QAction::triggered, this, [this] { setConfigMode(true, false); });
    connect(ui_->actionDefaultInheritance, &QAction::triggered, this, [this] { setConfigMode(true, true); });
    auto* icons = new QMenu(this); icons->setObjectName("connectionIconMenu");
    ui_->actionConnectionIcon->setMenu(icons); ui_->actionConnectionIcon->setToolTip("Choose connection icon");
    connect(icons, &QMenu::aboutToShow, this, [this, icons] {
        editor_->flush(); icons->clear(); const ProfileReference owner{editorDocument_, editor_->id()}; const bool defaults = editor_->defaultsMode();
        for (const auto& option : ConnectionIcons::choices(editor_->icon())) {
            const auto entry = option.toMap(); const auto name = entry.value("value").toString();
            auto* action = icons->addAction(qvariant_cast<QIcon>(entry.value("icon")), entry.value("text").toString());
            action->setData(name); action->setCheckable(true); action->setChecked(name == editor_->icon());
            connect(action, &QAction::triggered, this, [this, owner, name, defaults] {
                guard([this, owner, name, defaults] {
                    if (defaults) { if (editor_->defaultsMode()) editor_->setIcon(name); }
                    else if (!editor_->defaultsMode() && selectConnectionProfile(owner)) editor_->setIcon(name);
                });
            });
        }
    });
    if (auto* button = qobject_cast<QToolButton*>(ui_->configToolbar->widgetForAction(ui_->actionConnectionIcon))) button->setPopupMode(QToolButton::InstantPopup);
    connect(ui_->actionTabFullscreen, &QAction::triggered, this, [this] {
        auto* tabs = currentSessions(); fullscreenPane(tabs ? qobject_cast<SessionPane*>(tabs->currentWidget()) : nullptr);
    });
    connect(ui_->actionTabMinimize, &QAction::triggered, this, [this] { if (currentSessions() && currentSessions()->currentWidget()) showMinimized(); });
    configureTabs(ui_->sessionTabs);
    configureMenus();
    connect(ui_->connectionTree, &ConnectionTree::dragStarting, editor_, &ProfileEditor::flush);
    connect(ui_->connectionTree, &QTreeWidget::currentItemChanged, this, &MainWindow::selectionChanged);
    // Double-click opens a connection; on a folder it only expands/collapses.
    // Folders connect their contents only through the context-menu Connect.
    connect(ui_->connectionTree, &QTreeWidget::itemDoubleClicked, this, [this] {
        if (profiles_.value(selectedId()).value("kind") == "connection" && ui_->actionTreeConnect->isEnabled()) connectSelected(false);
    });
    connect(ui_->actionExpandAll, &QAction::triggered, ui_->connectionTree, &QTreeWidget::expandAll);
    connect(ui_->actionCollapseAll, &QAction::triggered, ui_->connectionTree, &QTreeWidget::collapseAll);
    connect(ui_->actionSort, &QAction::triggered, this, [this] { sortConnections(Qt::AscendingOrder); });
    connect(ui_->searchEdit, &QLineEdit::textChanged, this, &MainWindow::filterConnections);
    auto bindDock = [this](QDockWidget* dock, QAction* action) {
        connect(action, &QAction::toggled, dock, [dock](bool visible) { setShellVisible(dock, visible); });
        connect(dock, &QDockWidget::visibilityChanged, action, [dock, action] {
            if (dock->property("presentationSuppressed").toBool()) return;
            const QSignalBlocker blocker(action); action->setChecked(!dock->isHidden());
        });
    };
    bindDock(ui_->connectionsDock, ui_->actionViewConnections); bindDock(ui_->configDock, ui_->actionViewConfig);
    bindDock(ui_->notificationsDock, ui_->actionViewNotifications);
    auto bindToolbar = [this](QToolBar* toolbar, QAction* action) {
        connect(action, &QAction::toggled, toolbar, [toolbar](bool visible) { setShellVisible(toolbar, visible); });
        connect(toolbar, &QToolBar::visibilityChanged, action, [toolbar, action] {
            if (toolbar->property("presentationSuppressed").toBool()) return;
            const QSignalBlocker blocker(action); action->setChecked(!toolbar->isHidden());
        });
    };
    bindToolbar(ui_->quickConnectToolbar, ui_->actionViewQuickConnect); bindToolbar(ui_->externalToolsToolbar, ui_->actionViewExternalTools);
    bindToolbar(ui_->multiSshToolbar, ui_->actionViewMultiSsh);
    ui_->externalToolsToolbar->hide(); ui_->multiSshToolbar->hide();
    ui_->actionViewPanels->setToolTip("Show the connection panel tabs (General, Panel 1, …) above the session tabs");
    connect(ui_->actionViewPanels, &QAction::toggled, this, &MainWindow::applyPanelTabs);
    applyPanelTabs();
    connect(ui_->actionGeneralPanel, &QAction::triggered, this, [this] { ui_->groupTabs->setCurrentWidget(ui_->generalPanel); });
    ui_->configHelpTitle->setText("Profile settings");
    ui_->configHelpText->setText("Enter or leaving the cell saves it. Escape cancels the cell. Passwords belong to the selected XML file.");
    ui_->configSplitter->setSizes({170, 65}); ui_->notificationSplitter->setSizes({226, 899});
    setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea); setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() == "cocoa") placeQuickConnectInTitleBar();
#endif
    resetLayout();
    pollTimer_ = new QTimer(this);
    connect(pollTimer_, &QTimer::timeout, this, [this] {
        auto poll = [this](rm::Core& core, DocumentId document) {
            const auto events = core.poll_events();
            for (const auto& event : events) for (auto* pane : sessionPages()) {
                if (pane->document() != document || pane->id() != qstr(event.id)) continue;
                QPointer<SessionPane> guarded = pane;
                const QString kind = qstr(event.kind), text = qstr(event.text);
                QMetaObject::invokeMethod(pane, [guarded,kind,text] { if (guarded) guarded->event(kind,text); }, Qt::QueuedConnection);
            }
        };
        poll(*core_, {});
        for (const auto& doc : documents_) poll(doc->core(), doc->id());
    });
    initializeFiles();
    pollTimer_->start(16);
    preferences_ = new ApplicationPreferences(preferencesPath, this);
    // Only the real application downloads Cisco's decoder, beside its preferences.
    if (!testMode_ && !preferencesPath.isEmpty()) openh264_ = new OpenH264Codec(QFileInfo(preferencesPath).absolutePath(), this);
    ui_->actionOptions->setShortcut(preferencesShortcut());
    connect(ui_->actionOptions, &QAction::triggered, this, &MainWindow::showOptions);
    preferences_->load([this, preferencesPath](PreferenceReply reply) {
        if (closeApproved_) return;
        preferencesReady_ = reply.result.ok && !preferencesPath.isEmpty();
        if (reply.result.ok) applyPreferences(reply.values); else { fail(reply.result.error); updateActions(); }
        ui_->actionOptions->setEnabled(true); ui_->actionOptions->setToolTip("Options");
    });
}
void MainWindow::setConfigMode(bool defaults, bool inheritance) {
    if (defaults ? !preferencesReady_ || !documentRootSelected() : inheritance && !inheritanceAvailable()) return;
    guard([this, defaults, inheritance] {
        if (editor_->defaultsMode() == defaults) editor_->setInheritance(inheritance);
        else if (defaults) editor_->loadDefaults(defaults_, inheritance);
        else {
            editor_->load({}, {}); editor_->setInheritance(inheritance); properties();
        }
        updateActions();
    });
}
void MainWindow::applyPreferences(PreferenceValues values) {
    defaults_ = values.defaults;
    openh264Enabled_ = values.openh264;
    if (openh264_) openh264_->setEnabled(values.openh264);
    editor_->setCategorized(values.categorized); ui_->actionShowHelp->setChecked(values.propertyHelp);
    ui_->actionViewQuickConnect->setChecked(values.quickConnect); ui_->actionLockToolbars->setChecked(values.lockToolbars);
    ui_->actionViewPanels->setChecked(values.showPanels);
    commandAsControl_ = values.commandAsControl; DesktopView::setCommandAsControl(values.commandAsControl);
    autoUpdate_ = values.autoUpdate; if (updater_) updater_->setEnabled(values.autoUpdate);
    updateActions();
}
PreferenceValues MainWindow::currentPreferences() const {
    PreferenceValues values;
    values.categorized = editor_->categorized(); values.propertyHelp = ui_->actionShowHelp->isChecked();
    values.quickConnect = ui_->actionViewQuickConnect->isChecked(); values.lockToolbars = ui_->actionLockToolbars->isChecked();
    values.defaults = defaults_; values.openh264 = openh264Enabled_; values.showPanels = ui_->actionViewPanels->isChecked();
    values.commandAsControl = commandAsControl_; values.autoUpdate = autoUpdate_;
    return values;
}
void MainWindow::applyPanelTabs() {
    // Separate windows keep their panel row: it carries their window menu.
    setShellVisible(ui_->groupTabs->tabBar(), ui_->actionViewPanels->isChecked());
}
void MainWindow::showOptions() {
    guard([this] {
        if (optionsDialog_) { optionsDialog_->raise(); optionsDialog_->activateWindow(); return; }
        auto* dialog = new OptionsDialog(currentPreferences(), preferences_, this, openh264_); optionsDialog_ = dialog;
        connect(dialog, &OptionsDialog::applied, this, &MainWindow::applyPreferences);
        dialog->open();
    });
}
MainWindow::~MainWindow() {
    // Child widgets can emit focus/editor signals during QMainWindow teardown.
    closeApproved_ = true; closeRequested_ = false; cancelCredentials();
    delete dropHighlight_;
    for (auto* child : findChildren<QObject*>()) child->disconnect(this);
    const auto windows = panelWindows_ + retiredWindows_;
    // Sessions first: their views must be destroyed while the windows' GPU contexts exist.
    closeAllSessions(); for (const auto& window : windows) delete window.data(); delete files_; files_ = nullptr; documents_.clear();
}
void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && ui_) {
        const QSignalBlocker blocker(ui_->actionFullscreen); ui_->actionFullscreen->setChecked(isFullScreen());
    }
}
void MainWindow::closeEvent(QCloseEvent* event) {
    if (!closeApproved_) editor_->flush();
    if (!closeApproved_ && (autoSaveQueued_ || autoSaveInFlight_ || editor_->dirty())) {
        event->ignore(); guard([this] { close(); }); return;
    }
    if (!closeApproved_) for (const auto& doc : documents_) {
        if (doc->status().busy || !doc->status().warning.isEmpty() || doc->pending()) { closeRequested_ = true; event->ignore(); return; }
    }
    if (!closeApproved_ && transitionPending_) {
        closeRequested_ = true; event->ignore(); return;
    }
    closeApproved_ = true; cancelCredentials();
    closeAllSessions();
    if (sshTrust_ && !sshTrustStopped_) {
        sshTrust_->shutdown([this] { sshTrustStopped_ = true; finishSshShutdown(); });
    }
    if (terminalAppearance_ && !appearanceStopped_) {
        if (appearanceDialog_) { appearanceDialog_->hide(); appearanceDialog_->deleteLater(); }
        terminalAppearance_->shutdown([this] { appearanceStopped_ = true; finishSshShutdown(); });
    }
    if (!sshWorkers_.isEmpty() || !vncWorkers_.isEmpty() || VncWorker::activeWorkers() != 0 || !sshTrustStopped_ || !appearanceStopped_) { event->ignore(); return; }
    if (files_) for (auto id : documentOrder_) files_->lock(id);
    if (updater_ && updater_->ready() && autoUpdate_) {
        QString error;
        if (!updater_->install(relaunchAfterUpdate_, &error)) notification("Update not installed: " + error);
    }
    event->accept();
}
void MainWindow::finishSshShutdown() {
    if (closeApproved_ && sshWorkers_.isEmpty() && vncWorkers_.isEmpty() && VncWorker::activeWorkers() == 0 && sshTrustStopped_ && appearanceStopped_) QTimer::singleShot(0, this, [this] { close(); });
}
void MainWindow::showTerminalAppearance() {
    if (closeApproved_) return;
    if (!terminalAppearance_) { fail("Terminal appearance storage path is unavailable."); return; }
    if (!appearanceDialog_) {
        appearanceDialog_ = new TerminalAppearanceDialog(terminalAppearance_, this);
        appearanceDialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    appearanceDialog_->show(); appearanceDialog_->raise(); appearanceDialog_->activateWindow();
}
void MainWindow::finishTransition() {
    transitionPending_ = false; updateActions();
    if (closeRequested_ && !autoSaveInFlight_ && !closeApproved_) {
        closeRequested_ = false; QTimer::singleShot(0, this, [this] { close(); });
    }
}
void MainWindow::guard(std::function<void()> next) {
    if (closeApproved_ || ((transitionPending_) && !autoSaveInFlight_)) return;
    editor_->flush();
    if (autoSaveInFlight_ || autoSaveQueued_) {
        if (!pendingTransition_) pendingTransition_ = std::move(next);
        return;
    }
    if (!editor_->dirty()) { next(); return; }
    if (autoSaveError_) { recoverSave(std::move(next)); return; }
    pendingTransition_ = std::move(next); queueAutoSave();
}
void MainWindow::queueAutoSave() {
    if (closeApproved_ || !editor_->dirty() || autoSaveInFlight_) return;
    autoSaveError_ = false;
    const auto committed = profiles_.value(editor_->id()), draft = editor_->draft();
    if (!editor_->defaultsMode() && credentialAttempt_ && credentialAttempt_->document == editorDocument_ && (draft.value("overrides") != committed.value("overrides") || draft.value("parent") != committed.value("parent"))) {
        QString ancestor = credentialAttempt_->profile;
        while (!ancestor.isEmpty()) {
            if (ancestor == editor_->id()) { cancelCredentials(); break; }
            ancestor = profiles_.value(ancestor).value("parent").toString();
        }
    }
    if (autoSaveQueued_) return;
    autoSaveQueued_ = true;
    // Delegate commit/close and any initiating command must finish first.
    QTimer::singleShot(0, this, &MainWindow::startAutoSave);
}
void MainWindow::startAutoSave() {
    if (!autoSaveQueued_ || autoSaveInFlight_ || closeApproved_) return;
    if (transitionPending_ || (!editor_->defaultsMode() && context(editorDocument_) && context(editorDocument_)->status().busy)) return;
    autoSaveQueued_ = false;
    if (!editor_->dirty()) { autoSaveFinished(false); return; }
    autoSaveInFlight_ = true; statusBar()->showMessage("Saving…");
    saveDraft({});
}
void MainWindow::autoSaveFailed(const QString& error) {
    autoSaveQueued_ = false; autoSaveInFlight_ = false; autoSaveError_ = true;
    pendingTransition_ = {}; closeRequested_ = false; finishTransition();
    fail(error.isEmpty() ? "Save canceled. The pending value has not been saved." : error);
}
void MainWindow::autoSaveFinished(bool canceled) {
    autoSaveInFlight_ = false; autoSaveQueued_ = false; autoSaveError_ = false;
    auto next = std::move(pendingTransition_); pendingTransition_ = {};
    if (!editor_->dirty()) {
        properties();
        if (!canceled) statusBar()->showMessage(editor_->defaultsMode() ? "Connection defaults saved" : "Connection XML saved", 3000);
    }
    if (canceled) { closeRequested_ = false; next = {}; }
    updateActions(); if (next && !closeApproved_) next();
}
void MainWindow::recoverSave(std::function<void()> next) {
    if (saveRecovery_) { saveRecovery_->raise(); return; }
    transitionPending_ = true; updateActions();
    auto* dialog = new QMessageBox(QMessageBox::Warning, editor_->defaultsMode() ? "Defaults not saved" : "Connection not saved", "The pending value could not be saved. Retry, revert to the saved value, or stay here?",
                                  QMessageBox::Retry | QMessageBox::Discard | QMessageBox::Cancel, this);
    dialog->setObjectName("saveRecovery"); dialog->setTextFormat(Qt::PlainText); dialog->button(QMessageBox::Discard)->setText("Revert");
    dialog->setDefaultButton(QMessageBox::Cancel); dialog->setEscapeButton(QMessageBox::Cancel);
    dialog->setWindowModality(Qt::WindowModal); dialog->setAttribute(Qt::WA_DeleteOnClose); saveRecovery_ = dialog;
    connect(dialog, &QDialog::finished, this, [this, next = std::move(next)](int answer) mutable {
        saveRecovery_.clear(); finishTransition();
        if (answer == QMessageBox::Retry) { pendingTransition_ = std::move(next); queueAutoSave(); }
        else if (answer == QMessageBox::Discard) { autoSaveError_ = false; revertEditor(); if (next) next(); }
    }); dialog->open();
}
void MainWindow::selectTree(const QString& id, bool document) {
    const QSignalBlocker blocker(ui_->connectionTree);
    for (QTreeWidgetItemIterator it(ui_->connectionTree); *it; ++it) {
        const auto value = (*it)->data(0, Qt::UserRole);
        if ((document && (*it)->type() == DocumentItem && value.value<DocumentRootReference>().document == editorDocument_) ||
            (!document && (*it)->type() == ProfileItem && value.value<ProfileReference>() == ProfileReference{editorDocument_, id})) { ui_->connectionTree->setCurrentItem(*it); return; }
    }
}
void MainWindow::selectionChanged() {
    if (editor_->defaultsMode()) {
        // Defaults belong to the connections file; selecting anything else returns to Properties.
        if (documentRootSelected() || !ui_->connectionTree->currentItem()) { syncSelectedFile(); updateActions(); return; }
        setConfigMode(false, false); return;
    }
    const auto target = selectedId(); const bool document = documentRootSelected();
    const bool empty = !ui_->connectionTree->currentItem();
    const auto owner = selectedDocument();
    if (owner == editorDocument_ && target == editor_->id() && document == editor_->documentRoot()) return;
    if (!editorDocument_.valid()) { properties(); return; }
    selectTree(editor_->id(), editor_->documentRoot());
    guard([this, owner, target, document, empty] {
        if (empty) { const QSignalBlocker blocker(ui_->connectionTree); ui_->connectionTree->setCurrentItem(nullptr); properties(); }
        else if (document) selectConnectionFile(owner);
        else selectConnectionProfile({owner, target});
    });
}
void MainWindow::previewDraft() {
    if (!editor_->id().isEmpty()) {
        try {
            const auto effective = activeCore().preview_profile(bytes(QString::fromUtf8(QJsonDocument(editor_->draft()).toJson(QJsonDocument::Compact))));
            editor_->preview(QJsonDocument::fromJson(qstr(effective).toUtf8()).object());
        } catch (const rust::Error&) { editor_->invalid(); }
    }
    updateActions();
}
QString MainWindow::selectedId() const {
    auto* item = ui_->connectionTree->currentItem();
    if (!item || item->type() != ProfileItem) return {};
    return item->data(0, Qt::UserRole).value<ProfileReference>().profile;
}
bool MainWindow::documentRootSelected() const {
    auto* item = ui_->connectionTree->currentItem(); return item && item->type() == DocumentItem;
}
// Only items inside a folder have something to inherit from.
bool MainWindow::inheritanceAvailable() const {
    const auto doc = context(selectedDocument()); const auto id = selectedId();
    return doc && !id.isEmpty() && !doc->profiles().value(id).value("parent").toString().isEmpty();
}
void MainWindow::fail(const QString& message) {
    lastError_ = message; statusBar()->showMessage(message); notification(message, Notice::Error);
    if (!testMode_) {
        auto* dialog = new QMessageBox(QMessageBox::Warning, "CMRemote", message, QMessageBox::Ok, this);
        dialog->setTextFormat(Qt::PlainText); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->open();
    }
}
void MainWindow::rebuild(const QString& selected) {
    rebuildFiles(); if (!selected.isEmpty()) selectConnectionProfile({editorDocument_, selected});
}
void MainWindow::properties() {
    if (editor_->defaultsMode()) {
        if (documentRootSelected() || !ui_->connectionTree->currentItem()) { syncSelectedFile(); updateActions(); return; }
        editor_->load({}, {}); editor_->setInheritance(false); // a connection or folder was selected
    }
    const bool sameDocument = editorDocument_ == selectedDocument();
    const bool root = documentRootSelected(); const auto id = selectedId();
    const bool same = sameDocument && editor_->documentRoot() == root && (root || editor_->id() == id);
    if (same && (editor_->dirty() || editor_->editing())) { updateActions(); return; }
    syncSelectedFile();
    if (root) {
        const auto doc = context(editorDocument_);
        if (doc) editor_->loadDocument(doc->status().passwordProtected, doc->status().fullEncryption, documentHelp(), doc->canWrite());
        else editor_->loadDocument({}, false, documentHelp(), false);
    } else if (same) editor_->adopt(profiles_.value(id), profiles_);
    else editor_->load(profiles_.value(id), profiles_);
    if (editor_->inheritance() && !inheritanceAvailable()) editor_->setInheritance(false);
    updateActions();
}
void MainWindow::revertEditor() {
    if (editor_->defaultsMode()) { editor_->loadDefaults(defaults_, editor_->inheritance()); return; }
    pendingPassword_.reset(); editor_->finishPasswordEdit();
    if (editor_->documentRoot()) {
        const auto doc = context(editorDocument_);
        if (doc) editor_->loadDocument(doc->status().passwordProtected, doc->status().fullEncryption, documentHelp(), doc->canWrite());
        else editor_->loadDocument({}, false, documentHelp(), false);
    } else editor_->adopt(profiles_.value(editor_->id()), profiles_);
}
void MainWindow::updateActions() {
    const bool open = context(selectedDocument()) && context(selectedDocument())->status().metadataAvailable;
    const bool selected = open && !selectedId().isEmpty();
    const bool editable = open && !transitionPending_;
    ui_->actionCategorized->setChecked(editor_ && editor_->categorized()); ui_->actionAlphabetical->setChecked(editor_ && !editor_->categorized());
    const bool defaults = editor_ && editor_->defaultsMode();
    ui_->actionProperties->setChecked(!defaults && !editor_->inheritance()); ui_->actionInheritance->setChecked(!defaults && editor_->inheritance());
    ui_->actionDefaultProperties->setChecked(defaults && !editor_->inheritance()); ui_->actionDefaultInheritance->setChecked(defaults && editor_->inheritance());
    const bool defaultsAvailable = preferencesReady_ && !transitionPending_ && documentRootSelected();
    ui_->actionProperties->setEnabled(true); ui_->actionInheritance->setEnabled(!defaults && !transitionPending_ && inheritanceAvailable());
    ui_->actionDefaultProperties->setEnabled(defaultsAvailable); ui_->actionDefaultInheritance->setEnabled(defaultsAvailable);
    ui_->configTree->setEnabled((defaults || selected || documentRootSelected()) && !transitionPending_ && !autoSaveInFlight_); ui_->connectionTree->setEnabled(!transitionPending_);
    ui_->actionAddFolder->setEnabled(editable); ui_->actionAddConnection->setEnabled(editable);
    ui_->actionTreeRename->setEnabled(selected && editable); ui_->actionTreeDelete->setEnabled(selected && editable);
    const auto profile = profiles_.value(selectedId()), effective = profile.value("effective").toObject();
    bool rdpAvailable = false;
#ifdef RM_WITH_RDP
    rdpAvailable = true;
#endif
    ui_->quickConnectButton->setEnabled(!transitionPending_ && (rdpAvailable || quickConnectProtocol() == "ssh" || quickConnectProtocol() == "vnc"));
    ui_->actionTreeConnect->setEnabled(editable && !connectableProfiles(selectedDocument(), selectedId()).isEmpty());
    auto* tabs = currentSessions();
    auto* current = tabs ? qobject_cast<SessionPane*>(tabs->currentWidget()) : nullptr;
    ui_->actionConnectionIcon->setEnabled((defaults || selected) && !transitionPending_ && !autoSaveInFlight_ && !editor_->inheritance());
    ui_->actionConnectionIcon->setIcon(ConnectionIcons::resolve(editor_->icon(), profile.value("kind") == "folder"));
    ui_->actionSaveAsConnection->setEnabled(current && current->profile().isEmpty() && editable);
    const bool tab = current != nullptr;
    ui_->actionTabFullscreen->setEnabled(tab && !transitionPending_);
    ui_->actionTabFullscreen->setToolTip("Show this session fullscreen in its window; exit using the connection bar");
    ui_->actionTabMinimize->setEnabled(tab); ui_->actionTabMinimize->setToolTip("Minimize the window; keep the session connected");
    ui_->actionTabDisconnect->setEnabled(tab && current->active() && !transitionPending_); ui_->actionTabReconnect->setEnabled(tab && !transitionPending_);
    ui_->actionNextSession->setEnabled(tab); ui_->actionPreviousSession->setEnabled(tab);
    bool associated = false;
    for (auto* pane : sessionPages()) associated = associated || (pane->profile() == selectedId() && pane->document() == selectedDocument() && selected && pane->active());
    ui_->actionTreeDisconnect->setEnabled(associated && !transitionPending_);
    updateFileActions();
}
void MainWindow::sortConnections(Qt::SortOrder order) {
    editor_->flush(); treeOrder_ = order; ui_->connectionTree->sortItems(0, order);
}
void MainWindow::filterConnections() {
    const auto query = ui_->searchEdit->text().trimmed();
    for (QTreeWidgetItemIterator it(ui_->connectionTree); *it; ++it) (*it)->setHidden(false);
    if (query.isEmpty()) return;
    for (QTreeWidgetItemIterator it(ui_->connectionTree); *it; ++it) (*it)->setHidden((*it)->type() != DocumentItem);
    for (QTreeWidgetItemIterator it(ui_->connectionTree); *it; ++it) {
        auto* item = *it;
        QJsonObject profile;
        if (item->type() == ProfileItem) {
            const auto ref = item->data(0, Qt::UserRole).value<ProfileReference>();
            if (const auto doc = context(ref.document)) profile = doc->profiles().value(ref.profile);
        }
        const auto host = profile.value("effective").toObject().value("host").toString();
        if (!item->text(0).contains(query, Qt::CaseInsensitive) && !host.contains(query, Qt::CaseInsensitive)) continue;
        for (auto* ancestor = item; ancestor; ancestor = ancestor->parent()) ancestor->setHidden(false);
    }
}
void MainWindow::add(bool folder) {
    if (documentState_ != DocumentState::Open) return;
    const auto document = selectedDocument(); const auto id = selectedId(); const bool root = documentRootSelected();
    guard([this, folder, document, id, root] {
        if (!(root ? selectConnectionFile(document) : selectConnectionProfile({document, id}))) return;
        QString parent = selectedId(); if (profiles_.value(parent).value("kind") != "folder") parent = profiles_.value(parent).value("parent").toString();
        auto overrides = defaults_.overrides(); if (!folder) overrides["host"] = "";
        const QJsonObject request{{"name", folder ? "New folder" : "New connection"}, {"kind", folder ? "folder" : "connection"},
            {"parent", parent.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(parent)}, {"overrides", overrides}};
        writeFileProfile(rm::ProfileChange::Create, QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact)), [this, document] {
            if (editorDocument_ == document && !editor_->defaultsMode()) editor_->focusName();
        }, !parent.isEmpty() && defaults_.inherits("password"));
    });
}
void MainWindow::addFolder() { add(true); }
void MainWindow::addConnection() { add(false); }
void MainWindow::saveDraft(std::function<void()> next) {
    if (editor_->defaultsMode()) {
        auto values = currentPreferences(); values.defaults = editor_->defaultsDraft();
        updateActions();
        preferences_->save(values, [this, next = std::move(next)](PreferenceReply reply) {
            if (!reply.result.ok) { autoSaveFailed(reply.result.error); return; }
            defaults_ = reply.values.defaults; editor_->acceptDefaults(); lastError_.clear();
            notification("Connection defaults saved");
            autoSaveFinished(false);
            if (!reply.result.warning.isEmpty()) fail(reply.result.warning);
            if (next && !closeApproved_) next();
        });
        return;
    }
    if (editor_->documentRoot()) {
        const auto doc = context(editorDocument_); if (!doc) return;
        if (doc->status().passwordProtected != editor_->requestedProtection()) passwordProtectFile(editor_->requestedProtection());
        else protectFile(editor_->requestedFullEncryption());
        return;
    }
    if (editor_->passwordPending()) { savePasswordEdit(std::move(next)); return; }
    writeProfile(rm::ProfileChange::Update, QString::fromUtf8(QJsonDocument(editor_->draft()).toJson(QJsonDocument::Compact)), std::move(next));
}
void MainWindow::saveSelected() {
    if (transitionPending_ || (editor_->id().isEmpty() && !editor_->documentRoot() && !editor_->defaultsMode())) return;
    editor_->flush(); if (autoSaveError_) recoverSave({}); else queueAutoSave();
}
void MainWindow::connectSelected(bool mock) {
    const ProfileReference ref{selectedDocument(), selectedId()};
    guard([this, ref, mock] { if (selectConnectionProfile(ref)) connectCommitted(mock); });
}
void MainWindow::connectCommitted(bool mock) {
    const auto id = selectedId(); if (id.isEmpty()) return;
    if (profiles_.value(id).value("kind") == "folder") {
        const auto members = connectableProfiles(editorDocument_, id, mock);
        if (members.isEmpty()) { fail("This folder has no connections that can be opened."); return; }
        folderConnect_ = FolderConnect{editorDocument_, id, members, currentSessions(), mock};
        continueFolderConnect(); return;
    }
    for (auto* pane : sessionPages())
        if (pane->profile() == id && pane->document() == editorDocument_) { focusPane(pane); return; }
    const auto effective = profiles_.value(id).value("effective").toObject();
    if (!mock && effective.value("protocol") != "rdp" && effective.value("protocol") != "ssh" && effective.value("protocol") != "vnc") { fail("This protocol has no network backend."); return; }
    if (profiles_.value(id).value("kind") != "connection") { fail("Folders cannot open sessions"); return; }
    if (!effective.value("unsupported").toArray().isEmpty()) { fail("This native connection has unsupported settings; edit them before connecting."); return; }
    requestSession(id, sessionTarget(effective), mock, currentSessions(), nullptr, false, editorDocument_);
}
QStringList MainWindow::connectableProfiles(DocumentId document, const QString& id, bool mock) const {
    const auto doc = context(document); if (!doc) return {};
    bool rdp = mock;
#ifdef RM_WITH_RDP
    rdp = true;
#endif
    const auto connectable = [&](const QJsonObject& profile) {
        const auto effective = profile.value("effective").toObject(); const auto protocol = effective.value("protocol");
        return profile.value("kind") == "connection" && (protocol == "ssh" || protocol == "vnc" || (rdp && protocol == "rdp")) &&
            effective.value("unsupported").toArray().isEmpty() && !effective.value("host").toString().isEmpty();
    };
    const auto profile = doc->profiles().value(id);
    if (profile.value("kind") != "folder") return connectable(profile) ? QStringList{id} : QStringList{};
    QSet<QString> inside{id}; QStringList result;
    // File order lists parents before children, so one pass collects the subtree.
    for (const auto& candidate : doc->order()) {
        const auto node = doc->profiles().value(candidate);
        if (!inside.contains(node.value("parent").toString())) continue;
        inside.insert(candidate); if (connectable(node)) result.append(candidate);
    }
    return result;
}
void MainWindow::continueFolderConnect() {
    // One credential attempt runs at a time; each finish or cancel resumes here.
    while (folderConnect_ && !closeApproved_ && !credentialDialog_ && !credentialAttempt_) {
        auto& run = *folderConnect_; const auto doc = context(run.document);
        if (!doc || run.pending.isEmpty() || !run.owner) { folderConnect_.reset(); return; }
        const auto id = run.pending.takeFirst(); bool open = false;
        for (auto* pane : sessionPages()) open = open || (pane->profile() == id && pane->document() == run.document);
        if (open) continue;
        const auto target = attemptTarget(*doc, id, run.folder); if (!target) continue;
        requestSession(id, *target, run.mock, run.owner, nullptr, false, run.document, run.folder);
    }
}
void MainWindow::quickConnect(const QString& endpoint, bool mock) {
    const auto protocol = quickConnectProtocol();
    guard([this, endpoint, mock, protocol] {
        const auto target = protocol == "vnc" ? parseVncTarget(endpoint) : protocol == "ssh" ? parseSshTarget(endpoint) : parseRdpTarget(endpoint);
        if (!target) { fail("Invalid target; use hostname, IPv4 or IPv6 with an optional port"); return; }
        const auto address = endpoint.trimmed();
        auto* history = ui_->quickConnectCombo;
        if (history->findText(address) < 0) history->addItem(address);
        while (history->count() > 20) history->removeItem(0);
        history->setCurrentText(address);
        requestSession({}, *target, mock, currentSessions());
    });
}
QString MainWindow::quickConnectProtocol() const { return ui_->quickConnectButton->property("protocol").toString(); }
void MainWindow::setQuickConnectProtocol(const QString& protocol) {
    for (auto* action : ui_->quickConnectButton->menu()->actions()) if (action->isEnabled() && action->data().toString() == protocol) {
        const auto previous = quickConnectProtocol();
        if (!previous.isEmpty() && previous != protocol) {
            const auto text = ui_->quickConnectCombo->currentText(), moved = followDefaultPort(text, previous, protocol);
            if (moved != text) ui_->quickConnectCombo->setCurrentText(moved);
        }
        action->setChecked(true); ui_->quickConnectButton->setText(action->text());
        ui_->quickConnectButton->setProperty("protocol", protocol); updateActions(); return;
    }
}
void MainWindow::rebuildConnectionsMenu(QMenu* menu) {
    menu->clear();
    for (auto id : documentOrder_) {
        const auto doc = context(id); if (!doc) continue;
        const auto title = "Connections — " + QFileInfo(doc->status().path).fileName();
        if (!doc->status().metadataAvailable) { menu->addAction(QIcon(":/original/ASPWebSite_16x.png"), title + " (locked)")->setEnabled(false); continue; }
        std::function<void(QMenu*, const QString&)> fill = [&](QMenu* parent, const QString& folder) {
            for (const auto& profile : doc->order()) {
                const auto node = doc->profiles().value(profile);
                if (node.value("parent").toString() != folder) continue;
                const auto name = node.value("name").toString();
                if (node.value("kind") == "folder") {
                    bool children = false;
                    for (const auto& other : doc->order()) children = children || doc->profiles().value(other).value("parent").toString() == profile;
                    if (!children) { parent->addAction(ConnectionIcons::profile(node), name)->setEnabled(false); continue; }
                    auto* sub = parent->addMenu(ConnectionIcons::profile(node), name); fill(sub, profile);
                    continue;
                }
                auto* action = parent->addAction(ConnectionIcons::profile(node), name);
                action->setEnabled(!connectableProfiles(id, profile).isEmpty());
                connect(action, &QAction::triggered, this, [this, reference = ProfileReference{id, profile}] {
                    guard([this, reference] { if (selectConnectionProfile(reference)) connectCommitted(false); });
                });
            }
        };
        auto* root = menu->addMenu(QIcon(":/original/ASPWebSite_16x.png"), title); fill(root, {});
        if (root->isEmpty()) root->addAction("(empty)")->setEnabled(false);
    }
    if (menu->isEmpty()) menu->addAction("No connection files open")->setEnabled(false);
}
void MainWindow::requestSession(const QString& profile, SessionTarget target, bool mock, QTabWidget* owner, SessionPane* replacement, bool promptOnly, DocumentId document, const QString& folder) {
    if (closeApproved_ || !owner) return;
    if (credentialDialog_ || credentialAttempt_) {
        if (credentialDialog_) credentialDialog_->raise();
        return;
    }
#ifndef RM_WITH_RDP
    if (!mock && target.protocol == "rdp") { fail("FreeRDP is disabled in this build"); return; }
#endif
    if (!mock || profile.isEmpty()) {
        const auto normalized = normalizeSessionTarget(target); if (!normalized) { fail("Invalid session target"); return; } target = *normalized;
    }
    if (!profile.isEmpty() && !context(document)) return;
    if (mock) { attachSession(profile, target, true, owner, replacement, std::nullopt, document); return; }
    beginFileCredentials(document, profile, target, owner, replacement, promptOnly, folder);
}
void MainWindow::attachSession(const QString& profile, SessionTarget target, bool mock, QPointer<QTabWidget> owner,
                               QPointer<SessionPane> replacement, std::optional<TransientCredentials> credentials, DocumentId document,
                               std::optional<rust::Box<rm::ssh::Authentication>> sshAuthentication,
                               std::optional<rust::Box<rm::vnc::Authentication>> vncAuthentication) {
    // A fullscreen session returns to its tab before it is replaced there.
    if (replacement) { exitFullscreenFor(replacement); owner = ownerOf(replacement); }
    if (!owner || closeApproved_ || !sessionPanels().contains(owner)) return;
    if (!replacement) for (auto* existing : sessionPages()) {
        if ((!profile.isEmpty() && existing->profile() == profile && existing->document() == document) ||
            (profile.isEmpty() && existing->profile().isEmpty() && existing->target() == target)) { focusPane(existing); return; }
    }
    if (!mock && target.protocol == "ssh") {
        if (!sshAuthentication || sshHostKeyPath_.isEmpty()) { fail("SSH authentication or host-key cache path is unavailable."); return; }
        if (!sshTrust_) { sshTrust_ = new SshHostKeyService(sshHostKeyPath_, this); sshTrustStopped_ = false; }
    }
    if (!mock && target.protocol == "vnc" && !vncAuthentication) { fail("Session-only VNC authentication is required."); return; }
    try {
        auto& core = sessionCore(document);
        auto info = profile.isEmpty() ? (target.protocol == "vnc" ? core.open_temporary_vnc_session(bridgeVncTarget(target), mock) :
            target.protocol == "ssh" ? core.open_temporary_ssh_session(bridgeSshTarget(target), mock) : core.open_temporary_session(bridgeTarget(target), mock)) :
            replacement ? core.reopen_session(bytes(replacement->id()), mock) : core.open_session(bytes(profile), mock);
        info.host = bytes(target.host); info.port = target.port; info.username = bytes(target.username); info.domain = bytes(target.domain); info.options = bridgeOptions(target.options);
        info.ssh_auth = bytes(target.sshAuth); info.ssh_key_path = bytes(target.sshKeyPath); info.view_only = target.protocol == "vnc" && target.viewOnly;
        if (target.protocol == "vnc") { info.username = std::string(); info.domain = std::string(); }
        int index = owner->count(); QString caption = (mock ? "MOCK · " : target.protocol.toUpper() + " · ") + qstr(info.title);
        if (replacement) {
            index = owner->indexOf(replacement); if (index < 0) { core.close_session(info.id); return; }
            caption = owner->tabText(index); replacement->shutdown(); sessionCore(replacement->document()).close_session(bytes(replacement->id()));
            owner->removeTab(index); delete replacement;
        }
        // Attach only after QRhiWidget has enabled its RHI configuration.
        auto* pane = new SessionPane(info, profile, nullptr, document, clipboardEndpoint_);
        pane->setProperty("sessionProtocol", qstr(info.protocol));
        if (auto* view = pane->findChild<TerminalView*>()) {
            if (terminalAppearance_) view->setAppearance(terminalAppearance_->current());
            connect(view, &TerminalView::appearanceRequested, this, &MainWindow::showTerminalAppearance);
            // Quick Connect sessions can be saved from beside Copy/Paste.
            if (profile.isEmpty()) {
                auto* save = new QAction("Save as Connection…", pane); save->setObjectName("actionSessionSaveAsConnection");
                connect(save, &QAction::triggered, this, [this, target = QPointer<SessionPane>(pane)] {
                    if (target && target->profile().isEmpty()) guard([this, target] { saveTemporaryToFile(target); });
                });
                view->setContextActions({save});
            }
        }
        owner->insertTab(index, pane, context(document) ? ConnectionIcons::profile(context(document)->profiles().value(profile)) : ConnectionIcons::resolve(""), caption); owner->setCurrentIndex(index);
        connect(pane, &SessionPane::input, this, [this, document](const QString& session, const QString& text) {
            try { sessionCore(document).mock_input(bytes(session), bytes(text)); } catch (const rust::Error&) {}
        });
        connect(pane, &SessionPane::resized, this, [this, document](const QString& session, QSize size) {
            try { sessionCore(document).mock_resize(bytes(session), size.width(), size.height()); } catch (const rust::Error&) {}
        });
        QPointer<SessionPane> guarded = pane;
        connect(pane, &SessionPane::notice, this, [this](const QString& text) { notification(text, Notice::Warning); });
        connect(pane, &SessionPane::state, this, [this, guarded, document](const QString& session, const QString& state) {
            if (!guarded || guarded->id() != session || !guarded->active()) return;
            if (state == "failed" || state == "disconnected") exitFullscreenFor(guarded);
            if (guarded->mock()) return;
            try { sessionCore(document).session_state(bytes(session), bytes(state)); } catch (const rust::Error&) {}
            const auto detail = guarded->statusDetail().trimmed();
            notification(describeSession(guarded) + ": " + state + (detail.isEmpty() || state == "connected" ? QString() : " — " + detail),
                         state == "failed" ? Notice::Error : state == "disconnected" ? Notice::Warning : Notice::Info);
            updateActions();
        });
        if (!mock && target.protocol == "ssh") {
            SshWorker* worker = nullptr;
            try { worker = pane->startSsh(std::move(*sshAuthentication), sshTrust_); }
            catch (const rust::Error& error) { closePane(pane); fail(QString::fromUtf8(error.what())); return; }
            if (!worker) { closePane(pane); fail("SSH worker could not start."); return; }
            if (worker) {
                sshWorkers_.insert(worker);
                connect(worker, &SshWorker::finished, this, [this, worker](quint64) { sshWorkers_.remove(worker); finishSshShutdown(); });
            }
        }
        if (!mock && target.protocol == "vnc") {
            VncWorker* worker = nullptr;
            try { worker = pane->startVnc(std::move(*vncAuthentication)); }
            catch (const std::exception&) { closePane(pane); fail("VNC worker could not start."); return; }
            if (!worker) { closePane(pane); fail("VNC worker could not start."); return; }
            vncWorkers_.insert(worker);
            connect(worker, &VncWorker::finished, this, [this, worker](quint64) { vncWorkers_.remove(worker); finishSshShutdown(); });
        }
#ifdef RM_WITH_RDP
        if (!mock && target.protocol == "rdp") pane->startRdp({target.host, target.username, QFileInfo(dataPath_).absolutePath() + "/freerdp",
                                   std::move(credentials->password), target.port, credentials->clipboard || target.options.clipboard, target.domain, target.options});
#endif
        focusPane(pane); notification(mock ? "Opened mock session " + describeSession(pane) : "Opening " + describeSession(pane) + (replacement ? " (reconnect)" : ""));
        if (fullscreenOnOpen_.removeAll(ProfileReference{document, profile})) fullscreenPane(pane);
        if (!reconnectQueue_.isEmpty()) QTimer::singleShot(0, this, [this] { continueReconnectAll(); });
        updateActions();
    } catch (const rust::Error& e) { fail(QString::fromUtf8(e.what())); }
}
void MainWindow::saveAsConnection() {
    auto* tabs = currentSessions();
    QPointer<SessionPane> pane = tabs ? qobject_cast<SessionPane*>(tabs->currentWidget()) : nullptr;
    if (!pane || !pane->profile().isEmpty()) return;
    guard([this, pane] { saveTemporaryToFile(pane); });
}
void MainWindow::closeTab(int index) {
    auto* tabs = currentSessions();
    if (tabs) closePane(qobject_cast<SessionPane*>(tabs->widget(index)));
}
void MainWindow::disconnectPane(SessionPane* pane) {
    if (!pane) return;
    exitFullscreenFor(pane);
    if (credentialPage_ == pane) cancelCredentials();
    if (pane->active() && !pane->mock()) notification("Disconnecting " + describeSession(pane));
    pane->shutdown();
    try { sessionCore(pane->document()).session_state(bytes(pane->id()), "disconnected"); } catch (const rust::Error&) {}
    updateActions();
}
void MainWindow::disconnectCurrent() {
    auto* tabs = currentSessions(); if (tabs) disconnectPane(qobject_cast<SessionPane*>(tabs->currentWidget()));
}
void MainWindow::reconnectCurrent() {
    auto* tabs = currentSessions(); if (tabs) reconnectPane(qobject_cast<SessionPane*>(tabs->currentWidget()));
}
void MainWindow::reconnectPane(QPointer<SessionPane> pane, bool promptOnly) {
    if (!pane) return;
    guard([this, pane, promptOnly] {
        if (!pane || !ownerOf(pane)) return;
        disconnectPane(pane);
        auto reopen = [this, pane, promptOnly] {
            if (!pane || !ownerOf(pane) || closeApproved_) return;
            const auto profile = pane->profile(); auto target = pane->target();
            if (!profile.isEmpty()) {
                const auto owningProfiles = context(pane->document()) ? context(pane->document())->profiles() : QHash<QString, QJsonObject>{};
                const auto effective = owningProfiles.value(profile).value("effective").toObject();
                if (!effective.value("unsupported").toArray().isEmpty()) { fail("This native connection has unsupported settings; edit them before reconnecting."); return; }
                if (!pane->mock() && effective.value("protocol") != "rdp" && effective.value("protocol") != "ssh" && effective.value("protocol") != "vnc") { fail("This protocol has no network backend."); return; }
                target = sessionTarget(effective);
            }
            requestSession(profile, target, pane->mock(), ownerOf(pane), pane, promptOnly, pane->document());
        };
        if (pane->vncRetiring()) pane->afterVncRetirement(std::move(reopen));
        else if (pane->sshRetiring()) pane->afterSshRetirement(std::move(reopen)); else reopen();
    });
}
void MainWindow::deleteSelected() {
    const ProfileReference ref{selectedDocument(), selectedId()};
    guard([this, ref] { if (selectConnectionProfile(ref)) deleteCommitted(); });
}
bool MainWindow::subtreeHasTab(const QString& id) const {
    for (auto* pane : sessionPages()) {
        if (pane->document() != editorDocument_) continue;
        QString ancestor = pane->profile();
        while (!ancestor.isEmpty()) {
            if (ancestor == id) return true;
            ancestor = profiles_.value(ancestor).value("parent").toString();
        }
    }
    return false;
}
void MainWindow::deleteCommitted() {
    const auto id = selectedId(); if (id.isEmpty()) return;
    if (subtreeHasTab(id)) { fail("Close all tabs in this profile subtree before deleting it"); return; }
    const auto document = editorDocument_;
    auto remove = [this, id, document] {
        if (!selectConnectionProfile({document, id})) return;
        if (subtreeHasTab(id)) { finishTransition(); fail("Close all tabs in this profile subtree before deleting it"); return; }
        writeProfile(rm::ProfileChange::Delete, id);
    };
    if (testMode_) { remove(); return; }
    transitionPending_ = true; updateActions();
    auto* dialog = new QMessageBox(QMessageBox::Question, "Delete profile", "Delete this profile and all its children?",
                                  QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog->setObjectName("deleteProfile"); dialog->setTextFormat(Qt::PlainText); dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setWindowModality(Qt::WindowModal); dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [this, remove](int answer) { if (answer == QMessageBox::Yes) remove(); else finishTransition(); });
    dialog->open();
}

void MainWindow::setUpdateSource(const QUrl& source) { delete updateChecker_; updateChecker_ = new UpdateChecker(this, source); }
void MainWindow::checkForUpdates() {
    if (!updateChecker_) updateChecker_ = new UpdateChecker(this);
    if (updateChecker_->running()) return;
    connect(updateChecker_, &UpdateChecker::finished, this, [this](bool ok, const Updates::Release& latest, const QString& error) {
        const auto current = Updates::currentVersion(); const QString releases = QString(Updates::Repository) + "/releases";
        const auto link = [](const QString& url, const QString& text) { return "<a href=\"" + url.toHtmlEscaped() + "\">" + text.toHtmlEscaped() + "</a>"; };
        QString text;
        if (!ok) text = "Could not check for updates: " + error.toHtmlEscaped() + "<br>" + link(releases, releases);
        else if (Updates::compareVersions(current, latest.version).value_or(0) < 0) {
            text = "CMRemote " + latest.version.toHtmlEscaped() + " is available. You have " + current + ".<br>";
            if (updater_ && Updates::installerFor(latest.assets, latest.version)) {
                text += "It is downloading now and installs when CMRemote restarts or quits.<br>" + link(latest.url, "Release notes");
                // An explicit check downloads even when automatic updates are off.
                updater_->checkNow();
            } else text += link(latest.url, "Download it from GitHub");
        }
        else text = "You have the latest version, CMRemote " + current + ".";
        auto* dialog = new QMessageBox(QMessageBox::NoIcon, "Check for Updates", text, QMessageBox::Ok, this);
        dialog->setObjectName("updateDialog"); dialog->setProperty("latestVersion", ok ? latest.version : QString());
        dialog->setIconPixmap(Branding::applicationIcon().pixmap(QSize(64, 64), dialog->devicePixelRatioF()));
        dialog->setTextFormat(Qt::RichText); dialog->setTextInteractionFlags(Qt::TextBrowserInteraction);
        if (auto* label = dialog->findChild<QLabel*>("qt_msgbox_label")) label->setOpenExternalLinks(true);
        dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->open();
    }, Qt::SingleShotConnection);
    updateChecker_->check();
}
void MainWindow::enableAutomaticUpdates(const QString& dataDirectory) {
    if (updater_ || testMode_) return;
    updater_ = new AutoUpdater(dataDirectory, this);
    connect(updater_, &AutoUpdater::log, this, [this](const QString& text) { notification(text); });
    connect(updater_, &AutoUpdater::readyToInstall, this, &MainWindow::promptUpdateInstall);
    updater_->setEnabled(autoUpdate_);
}
void MainWindow::promptUpdateInstall(const QString& version) {
    if (closeApproved_ || updatePrompted_) return;
    updatePrompted_ = true;
    auto* dialog = new QMessageBox(QMessageBox::NoIcon, "Update ready",
        "CMRemote " + version + " has been downloaded and verified.\n\nRestart now to install it, or keep working and it installs when you quit.",
        QMessageBox::NoButton, this);
    dialog->setObjectName("updateReady"); dialog->setTextFormat(Qt::PlainText);
    dialog->setIconPixmap(Branding::applicationIcon().pixmap(QSize(64, 64), dialog->devicePixelRatioF()));
    auto* restart = dialog->addButton("Restart Now", QMessageBox::AcceptRole); dialog->addButton("When I Quit", QMessageBox::RejectRole);
    dialog->setDefaultButton(restart); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowModality(Qt::NonModal);
    connect(dialog, &QMessageBox::buttonClicked, this, [this, restart](QAbstractButton* button) {
        if (button != restart) return;
        relaunchAfterUpdate_ = true; notification("Update: restarting to install CMRemote " + updater_->readyVersion() + ".");
        QTimer::singleShot(0, this, [this] { close(); });
    });
    dialog->show();
}
void MainWindow::placeQuickConnectInTitleBar() {
#ifdef Q_OS_MACOS
    // The window content extends under a transparent title bar; the toolbar row
    // starts right after the close/minimize/zoom buttons.
    setWindowFlag(Qt::ExpandedClientAreaHint, true); setWindowFlag(Qt::NoTitleBarBackgroundHint, true);
    // Qt otherwise keeps the layout below the title bar's safe area.
    setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    const auto inset = rmNativeTitleBarInset(this); if (inset.isEmpty()) return;
    quickConnectInTitleBar_ = true; titleBarHeight_ = inset.height();
    auto* toolbar = ui_->quickConnectToolbar;
    auto* spacer = new QWidget(toolbar); spacer->setObjectName("titleBarInset"); spacer->setFixedWidth(inset.width());
    spacer->setAttribute(Qt::WA_TransparentForMouseEvents);
    toolbar->insertWidget(toolbar->actions().value(0), spacer);
    toolbar->setMovable(false); toolbar->setMinimumHeight(titleBarHeight_);
    toolbar->setStyleSheet("QToolBar { background: transparent; border: none; }");
    // Empty toolbar space still drags and zooms the window like a title bar.
    for (QWidget* area : {static_cast<QWidget*>(toolbar), ui_->quickConnectControls, static_cast<QWidget*>(ui_->quickConnectLabel)}) area->installEventFilter(this);
    // Without the toolbar, keep everything below the window buttons.
    auto margins = [this] { setContentsMargins(0, ui_->quickConnectToolbar->isHidden() ? titleBarHeight_ : 0, 0, 0); };
    connect(toolbar, &QToolBar::visibilityChanged, this, margins); margins();
#endif
}
bool MainWindow::titleBarDrag(QObject* watched, QEvent* event) {
    if (!quickConnectInTitleBar_) return false;
    if (watched != ui_->quickConnectToolbar && watched != ui_->quickConnectControls && watched != ui_->quickConnectLabel) return false;
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (event->type() == QEvent::MouseButtonDblClick && mouse->button() == Qt::LeftButton) {
        if (isMaximized()) showNormal(); else showMaximized();
        return true;
    }
    if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton && windowHandle())
        return windowHandle()->startSystemMove();
    return false;
}
