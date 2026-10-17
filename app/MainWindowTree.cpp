#include "MainWindow.h"
#include "ConnectionTree.h"
#include "ProfileEditor.h"
#include "SessionPane.h"
#include "ui_test.h"
#include "Views.h"
#include "SshHostKeyService.h"
#include "SshTransferDialog.h"
#include <QLineEdit>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QTcpSocket>
#include <QTimer>

// Connection tree commands. The context menu mirrors mRemoteNG's: same items, order and shortcuts.
void MainWindow::setupTreeActions() {
    ui_->actionAddConnection->setText("New Connection");
    const std::pair<QAction*, QKeySequence> shortcuts[] = {
        {ui_->actionTreeConnect, QKeySequence("Ctrl+Shift+C")}, {ui_->actionTreeDuplicate, QKeySequence("Ctrl+D")},
        {ui_->actionTreeRename, QKeySequence(Qt::Key_F2)}, {ui_->actionTreeDelete, QKeySequence(Qt::Key_Delete)},
        {ui_->actionMoveUp, QKeySequence("Ctrl+Up")}, {ui_->actionMoveDown, QKeySequence("Ctrl+Down")}};
    for (const auto& [action, keys] : shortcuts) {
        action->setShortcut(keys); action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        ui_->connectionTree->addAction(action);
    }
    // File > Import / Export to File, as in mRemoteNG.
    ui_->actionImportFile->setText("Import from File…"); ui_->actionImportPutty->setText("Import from PuTTY…");
    auto* imports = new QMenu("Import", this); imports->setObjectName("menuImport");
    imports->addAction(ui_->actionImportFile); imports->addAction(ui_->actionImportPutty);
    ui_->menuFile->insertMenu(ui_->actionOptions, imports); ui_->menuFile->insertAction(ui_->actionOptions, ui_->actionExport);
    ui_->menuFile->insertSeparator(ui_->actionOptions);
    connect(ui_->actionImportFile, &QAction::triggered, this, &MainWindow::showImportDialog);
    connect(ui_->actionImportPutty, &QAction::triggered, this, &MainWindow::importPuttySessions);
    connect(ui_->actionExport, &QAction::triggered, this, &MainWindow::showExportDialog);
    // Tools > External Tools, the tree submenu and the External Tools toolbar share one list.
    externalToolsPath_ = QFileInfo(dataPath_).absolutePath() + "/external-tools.json";
    externalTools_ = ExternalTools::load(externalToolsPath_);
    ui_->actionExternalTools->setText("External Tools…"); ui_->actionExternalTools->setEnabled(true);
    ui_->actionExternalTools->setToolTip("Configure programs to launch for a connection");
    ui_->actionTreeExternalTools->setText("External Tools");
    connect(ui_->actionExternalTools, &QAction::triggered, this, &MainWindow::showExternalTools);
    rebuildExternalTools();
    // View > Multi SSH Toolbar: one command line typed into every connected SSH session.
    auto* multi = new QLineEdit(ui_->multiSshToolbar); multi->setObjectName("multiSshEdit");
    multi->setPlaceholderText("Command for all SSH sessions — press Enter to send"); multi->setMinimumWidth(320);
    ui_->multiSshToolbar->addWidget(multi);
    connect(multi, &QLineEdit::returnPressed, this, &MainWindow::sendMultiSsh);
    connect(ui_->actionTreeDuplicate, &QAction::triggered, this, &MainWindow::duplicateSelected);
    connect(ui_->actionMoveUp, &QAction::triggered, this, [this] { moveSelected(-1); });
    connect(ui_->actionMoveDown, &QAction::triggered, this, [this] { moveSelected(1); });
    connect(ui_->actionConsoleSession, &QAction::triggered, this, [this] { connectWithOption(ConnectOption::Console); });
    connect(ui_->actionConnectFullscreen, &QAction::triggered, this, [this] { connectWithOption(ConnectOption::Fullscreen); });
    connect(ui_->actionNoCredentials, &QAction::triggered, this, [this] { connectWithOption(ConnectOption::NoCredentials); });
    connect(ui_->actionChoosePanel, &QAction::triggered, this, [this] { connectWithOption(ConnectOption::ChoosePanel); });
}

QMenu* MainWindow::treeMenu(const QString& id, bool document) {
    auto* menu = new QMenu(this); menu->setObjectName("treeContextMenu"); const auto profile = profiles_.value(id);
    const auto owner = selectedDocument(); const auto doc = context(owner); if (!doc) return menu;
    auto selected = [this, owner, id, document](std::function<void()> command) {
        guard([this, owner, id, document, command = std::move(command)] { if (document ? selectConnectionFile(owner) : selectConnectionProfile({owner, id})) command(); });
    };
    const bool writable = doc->canWrite() && !transitionPending_;
    const bool connection = profile.value("kind") == "connection";
    // A folder connects or disconnects every connection beneath it.
    const bool canConnect = !id.isEmpty() && !connectableProfiles(owner, id).isEmpty();
    bool active = false; for (auto* pane : sessionPages()) active = active || (!id.isEmpty() && profileInScope(owner, id, pane));

    menuAction(menu, "actionTreeConnect", canConnect, [this, selected] { selected([this] { connectCommitted(false); }); });
    auto* options = menu->addMenu("Connect (with options)"); options->setObjectName("connectOptionsMenu");
    options->setEnabled(canConnect && connection);
    const bool rdp = profile.value("effective").toObject().value("protocol").toString() == "rdp";
    menuAction(options, "actionConsoleSession", rdp, [this] { connectWithOption(ConnectOption::Console); });
    menuAction(options, "actionConnectFullscreen", true, [this] { connectWithOption(ConnectOption::Fullscreen); });
    menuAction(options, "actionNoCredentials", true, [this] { connectWithOption(ConnectOption::NoCredentials); });
    menuAction(options, "actionChoosePanel", true, [this] { connectWithOption(ConnectOption::ChoosePanel); });
    menuAction(menu, "actionTreeDisconnect", active, [this, selected] { selected([this] { disconnectSelected(); }); });
    menu->addSeparator();
    auto* tools = menu->addMenu(ui_->actionTreeExternalTools->icon(), "External Tools"); tools->setObjectName("externalToolsMenu");
    for (int i = 0; i < externalTools_.size(); ++i) {
        auto* tool = tools->addAction(externalTools_.at(i).name); tool->setEnabled(connection);
        connect(tool, &QAction::triggered, this, [this, selected, i] { selected([this, i] { runExternalTool(i); }); });
    }
    if (!externalTools_.isEmpty()) tools->addSeparator();
    connect(tools->addAction("Configure External Tools…"), &QAction::triggered, this, [this, selected] { selected([this] { showExternalTools(); }); });
    const bool ssh = connection && profile.value("effective").toObject().value("protocol").toString() == "ssh";
    auto* transfer = menu->addAction(ui_->actionTabTransfer->icon(), "Transfer File (SSH)"); transfer->setObjectName("actionTreeTransfer");
    transfer->setEnabled(ssh);
    connect(transfer, &QAction::triggered, this, [this, owner, id] { showTransferFile({owner, id}); });
    menu->addSeparator();
    menuAction(menu, "actionTreeDuplicate", writable && !id.isEmpty(), [this, selected] { selected([this] { duplicateSelected(); }); });
    menuAction(menu, "actionTreeRename", writable && !id.isEmpty(), [this, selected] { selected([this] { editor_->focusName(); }); });
    menuAction(menu, "actionTreeDelete", writable && !id.isEmpty(), [this, selected] { selected([this] { deleteCommitted(); }); });
    menu->addSeparator();
    auto* imports = menu->addMenu("Import"); imports->setObjectName("importMenu"); imports->setEnabled(writable);
    menuAction(imports, "actionImportFile", true, [this, selected] { selected([this] { showImportDialog(); }); });
    menuAction(imports, "actionImportPutty", true, [this, selected] { selected([this] { importPuttySessions(); }); });
    menuAction(menu, "actionExport", doc->status().metadataAvailable, [this, selected] { selected([this] { showExportDialog(); }); });
    menu->addSeparator();
    menuAction(menu, "actionAddConnection", writable, [this, selected] { selected([this] { addConnection(); }); });
    menuAction(menu, "actionAddFolder", writable, [this, selected] { selected([this] { addFolder(); }); });
    menu->addSeparator();
    auto* sort = menu->addMenu("Sort"); sort->setObjectName("sortMenu");
    for (auto order : {Qt::AscendingOrder, Qt::DescendingOrder})
        menuAction(sort, order == Qt::AscendingOrder ? "actionSortAscending" : "actionSortDescending", true, [this, order] { sortConnections(order); });
    menuAction(menu, "actionMoveUp", writable && siblingMove(-1).has_value(), [this, selected] { selected([this] { moveSelected(-1); }); });
    menuAction(menu, "actionMoveDown", writable && siblingMove(1).has_value(), [this, selected] { selected([this] { moveSelected(1); }); });
    if (document) {
        // Connection files are a CMRemote addition; their commands follow mRemoteNG's items.
        menu->addSeparator();
        if (!doc->status().warning.isEmpty()) {
            auto* warning = menu->addAction("Acknowledge durability warning…"); warning->setObjectName("actionFileWarning");
            connect(warning, &QAction::triggered, this, [this, owner] { fileWarning(owner, {}); });
        }
        if (ui_->actionUnlockPasswords->isEnabled()) menuAction(menu, "actionUnlockPasswords", true, [this, owner] { promptFileUnlock(owner); });
        if (doc->status().unlocked || doc->status().compatibilityMaster || doc->status().busy)
            menuAction(menu, "actionLockPasswords", true, [this, owner] { files_->lock(owner); });
        if (ui_->actionChangeMaster->isEnabled()) menuAction(menu, "actionChangeMaster", true, [this, owner] { changeFileMaster(owner); });
        if (doc->canRepairIds() && !transitionPending_) menuAction(menu, "actionRepairIds", true, [this, owner] { repairFileIds(owner); });
        menuAction(menu, "actionSaveFile", writable, [this, selected] { selected([this] { saveConnectionFile(); }); });
        auto* close = menu->addAction("Close connection file"); close->setObjectName("actionCloseFile"); close->setEnabled(!doc->status().busy);
        connect(close, &QAction::triggered, this, [this, selected] { selected([this] { closeConnectionFile(); }); });
    }
    return menu;
}

bool MainWindow::profileInScope(DocumentId document, const QString& id, SessionPane* pane) const {
    if (pane->document() != document || pane->profile().isEmpty()) return false;
    const auto doc = context(document); if (!doc) return false;
    for (auto current = pane->profile(); !current.isEmpty(); current = doc->profiles().value(current).value("parent").toString())
        if (current == id) return true;
    return false;
}

void MainWindow::disconnectSelected() {
    // Disconnect ends the session and closes its tab, as in mRemoteNG.
    const auto document = selectedDocument(); const auto id = selectedId(); if (id.isEmpty()) return;
    for (auto* pane : sessionPages()) if (profileInScope(document, id, pane)) closePane(pane);
}

void MainWindow::connectWithOption(ConnectOption option) {
    const ProfileReference reference{selectedDocument(), selectedId()};
    if (reference.profile.isEmpty()) return;
    guard([this, reference, option] {
        if (!selectConnectionProfile(reference)) return;
        if (option == ConnectOption::ChoosePanel)
            choosePanel([this, reference](QTabWidget* owner) { connectInPanel(reference, owner, ConnectOption::ChoosePanel); });
        else connectInPanel(reference, currentSessions(), option);
    });
}

void MainWindow::connectInPanel(ProfileReference reference, QPointer<QTabWidget> owner, ConnectOption option) {
    if (!owner || !selectConnectionProfile(reference)) return;
    auto target = committedTarget(reference.profile); if (!target) { fail("Only connections can be opened."); return; }
    if (!profiles_.value(reference.profile).value("effective").toObject().value("unsupported").toArray().isEmpty()) {
        fail("This native connection has unsupported settings; edit them before connecting."); return;
    }
    for (auto* pane : sessionPages()) if (pane->profile() == reference.profile && pane->document() == reference.document) {
        if (option == ConnectOption::Fullscreen) fullscreenPane(pane); else focusPane(pane);
        return;
    }
    if (option == ConnectOption::Console) target->options.consoleSession = true;
    if (option == ConnectOption::Fullscreen) fullscreenOnOpen_.append(reference);
    requestSession(reference.profile, *target, false, owner, nullptr, option == ConnectOption::NoCredentials, reference.document);
}

void MainWindow::choosePanel(std::function<void(QTabWidget*)> chosen) {
    QStringList names; QList<QPointer<QTabWidget>> panels;
    for (auto* groups : groupBars()) for (int i = 0; i < groups->count(); ++i)
        if (auto* tabs = groups->widget(i) ? groups->widget(i)->findChild<QTabWidget*>() : nullptr) { names << groups->tabText(i); panels << tabs; }
    names << "New panel";
    auto* dialog = new QInputDialog(this); dialog->setObjectName("choosePanelDialog");
    dialog->setWindowTitle("Choose panel"); dialog->setLabelText("Connect in panel:");
    dialog->setComboBoxItems(names); dialog->setComboBoxEditable(false); dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, dialog, names, panels, chosen = std::move(chosen)] {
        const int index = names.indexOf(dialog->textValue()); if (index < 0) return;
        QTabWidget* owner = index < panels.size() ? panels.at(index).data() : createPanel(ui_->groupTabs)->findChild<QTabWidget*>();
        if (owner) chosen(owner);
    });
    dialog->open();
}

void MainWindow::duplicateSelected() {
    const ProfileReference reference{selectedDocument(), selectedId()};
    if (reference.profile.isEmpty()) return;
    guard([this, reference] {
        if (!selectConnectionProfile(reference)) return;
        writeFileProfile(rm::ProfileChange::Duplicate, reference.profile, {});
    });
}

std::optional<QJsonObject> MainWindow::siblingMove(int delta) const {
    const auto doc = context(selectedDocument()); const auto id = selectedId();
    if (!doc || id.isEmpty() || !doc->profiles().contains(id)) return std::nullopt;
    const auto parent = doc->profiles().value(id).value("parent").toString();
    QStringList siblings;
    for (const auto& profile : doc->order()) if (doc->profiles().value(profile).value("parent").toString() == parent) siblings << profile;
    const int from = siblings.indexOf(id), to = from + delta;
    if (from < 0 || to < 0 || to >= siblings.size()) return std::nullopt;
    // Move places the profile before `before`; moving down skips past the next sibling.
    const auto before = delta < 0 ? siblings.at(to) : to + 1 < siblings.size() ? siblings.at(to + 1) : QString();
    return QJsonObject{{"id", id}, {"parent", parent.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(parent)},
                       {"before", before.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(before)}};
}

void MainWindow::moveSelected(int delta) {
    const ProfileReference reference{selectedDocument(), selectedId()};
    if (reference.profile.isEmpty()) return;
    guard([this, reference, delta] {
        if (!selectConnectionProfile(reference)) return;
        const auto doc = context(reference.document); const auto request = siblingMove(delta);
        if (!doc || !doc->canWrite() || !request) return;
        treeOrder_.reset(); // A manual placement replaces any view-only A–Z/Z–A sort.
        writeFileProfile(rm::ProfileChange::Move, QString::fromUtf8(QJsonDocument(*request).toJson(QJsonDocument::Compact)), {});
    });
}

void MainWindow::reconnectAll() {
    reconnectQueue_.clear();
    for (auto* pane : sessionPages()) reconnectQueue_.append(pane);
    continueReconnectAll();
}

void MainWindow::continueReconnectAll() {
    // One tab at a time: each may ask for credentials, and only one prompt can be open.
    if (closeApproved_ || credentialDialog_ || credentialAttempt_ || folderConnect_) return;
    while (!reconnectQueue_.isEmpty()) {
        const auto pane = reconnectQueue_.takeFirst();
        if (pane && ownerOf(pane)) { reconnectPane(pane); return; }
    }
}

void MainWindow::checkHostStatus() {
    const auto id = selectedId(); const auto target = committedTarget(id); if (!target) return;
    const auto name = profiles_.value(id).value("name").toString(); const auto endpoint = target->endpoint();
    auto* socket = new QTcpSocket(this); auto* timeout = new QTimer(socket); timeout->setSingleShot(true);
    const auto report = [this, socket, name, endpoint](const QString& result) {
        if (socket->property("reported").toBool()) return;
        socket->setProperty("reported", true); notification(name + " (" + endpoint + ") " + result); socket->deleteLater();
    };
    connect(socket, &QTcpSocket::connected, this, [report, socket] { report("is reachable"); socket->abort(); });
    connect(socket, &QTcpSocket::errorOccurred, this, [report, socket] { report("is not reachable: " + socket->errorString()); });
    connect(timeout, &QTimer::timeout, this, [report, socket] { report("did not answer within 5 seconds"); socket->abort(); });
    timeout->start(5000); socket->connectToHost(target->host, target->port);
}

void MainWindow::rebuildExternalTools() {
    ui_->externalToolsToolbar->clear(); ui_->externalToolsToolbar->addAction(ui_->actionExternalTools);
    for (int i = 0; i < externalTools_.size(); ++i) if (externalTools_.at(i).toolbar) {
        auto* action = ui_->externalToolsToolbar->addAction(externalTools_.at(i).name);
        action->setToolTip("Launch " + externalTools_.at(i).name + " for the selected connection");
        connect(action, &QAction::triggered, this, [this, i] { runExternalTool(i); });
    }
}

void MainWindow::showExternalTools() {
    const auto id = selectedId();
    auto* dialog = new ExternalToolsDialog(externalTools_, id.isEmpty() ? QJsonObject{} : profiles_.value(id), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        externalTools_ = dialog->tools();
        if (!ExternalTools::save(externalToolsPath_, externalTools_)) fail("Cannot save external tools.");
        rebuildExternalTools();
    });
    dialog->open();
}

void MainWindow::runExternalTool(int index) {
    if (index < 0 || index >= externalTools_.size()) return;
    const auto profile = profiles_.value(selectedId());
    if (profile.value("kind") != "connection") { fail("Select a connection to launch " + externalTools_.at(index).name + "."); return; }
    QString error;
    if (ExternalTools::launch(externalTools_.at(index), profile, &error)) notification("Launched " + externalTools_.at(index).name);
    else fail(error);
}

void MainWindow::sendMultiSsh() {
    auto* edit = ui_->multiSshToolbar->findChild<QLineEdit*>("multiSshEdit"); if (!edit) return;
    int sent = 0, sessions = 0;
    for (auto* pane : sessionPages()) {
        if (pane->target().protocol != "ssh" || pane->mock() || !pane->active()) continue;
        ++sessions;
        if (auto* view = pane->findChild<TerminalView*>(); view && view->sendText(edit->text() + "\r")) ++sent;
    }
    if (!sessions) { fail("No SSH sessions are connected."); return; }
    notification(QString("Sent to %1 of %2 SSH session%3").arg(sent).arg(sessions).arg(sessions == 1 ? "" : "s"));
    edit->clear();
}

void MainWindow::showTransferFile(ProfileReference reference) {
    const auto doc = context(reference.document); if (!doc) return;
    const auto target = normalizeSessionTarget(sessionTarget(doc->profiles().value(reference.profile).value("effective").toObject()));
    if (!target || target->protocol != "ssh") { fail("Transfer File (SSH) needs an SSH connection."); return; }
    if (sshHostKeyPath_.isEmpty()) { fail("The SSH host-key cache is unavailable."); return; }
    if (!sshTrust_) { sshTrust_ = new SshHostKeyService(sshHostKeyPath_, this); sshTrustStopped_ = false; }
    auto* dialog = new SshTransferDialog(*target, sshTrust_, this); dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &SshTransferDialog::finished, this, [this](bool ok, QString message) { if (ok) notification(message); else fail(message); });
    // Prefill the saved password when this file is unlocked and its binding allows reuse.
    if (target->sshAuth == "password" && doc->status().unlocked) {
        const QPointer<SshTransferDialog> guarded(dialog);
        files_->lookup(reference, *target, [guarded](DocumentReply reply) {
            if (guarded && reply.result.ok && reply.password) guarded->setPassword(std::move(*reply.password));
        });
    }
    dialog->open();
}
