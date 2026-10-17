#include "MainWindow.h"
#include <QDir>
#include "VncAuthentication.h"
#include "ConnectionCrypto.h"
#include "ProfileEditor.h"
#include "ConnectionIcons.h"
#include "SessionPane.h"
#include "FilePasswordDialogs.h"
#include "ui_test.h"
#include <QFileDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QScrollBar>
#include <QStatusBar>
#include <QTimer>
#include <QTreeWidgetItemIterator>

namespace {
std::string bytes(const QString& text) { return text.toUtf8().toStdString(); }
}
std::shared_ptr<DocumentContext> MainWindow::context(DocumentId id) const { return documents_.value(id); }
rm::Core& MainWindow::activeCore() {
    const auto doc = context(editorDocument_); Q_ASSERT(doc); return doc->core();
}
rm::Core& MainWindow::sessionCore(DocumentId id) {
    if (id.valid()) { const auto doc = context(id); Q_ASSERT(doc); return doc->core(); }
    return *core_;
}
size_t MainWindow::sessionCount() const {
    size_t count = core_->session_count();
    for (const auto& doc : documents_) count += doc->core().session_count();
    return count;
}
DocumentId MainWindow::selectedDocument() const {
    const auto* item = ui_->connectionTree->currentItem(); if (!item) return {};
    const auto value = item->data(0, Qt::UserRole);
    if (item->type() == DocumentItem) return value.value<DocumentRootReference>().document;
    if (item->type() == ProfileItem) return value.value<ProfileReference>().document;
    return {};
}
bool MainWindow::selectConnectionFile(DocumentId id) { return selectFileReference(DocumentRootReference{id}); }
bool MainWindow::selectConnectionProfile(ProfileReference profile) { return profile.valid() && selectFileReference(profile); }
bool MainWindow::selectFileReference(const std::variant<DocumentRootReference, ProfileReference>& reference) {
    const auto* root = std::get_if<DocumentRootReference>(&reference);
    const auto id = root ? root->document : std::get<ProfileReference>(reference).document;
    const auto doc = context(id); if (!doc) return false;
    if (!root && (!std::get<ProfileReference>(reference).valid() || !doc->profiles().contains(std::get<ProfileReference>(reference).profile))) return false;
    // Programmatic selection follows the same draft settlement as a tree click.
    if (editor_->dirty() || autoSaveInFlight_ || autoSaveQueued_) {
        guard([this, reference] { selectFileReference(reference); }); return true;
    }
    const QSignalBlocker blocker(ui_->connectionTree);
    for (QTreeWidgetItemIterator it(ui_->connectionTree); *it; ++it) {
        const auto value = (*it)->data(0, Qt::UserRole);
        if ((root && (*it)->type() == DocumentItem && value.value<DocumentRootReference>().document == id) ||
            (!root && (*it)->type() == ProfileItem && value.value<ProfileReference>() == std::get<ProfileReference>(reference))) {
            ui_->connectionTree->setCurrentItem(*it); properties(); return true;
        }
    }
    return false;
}
void MainWindow::syncSelectedFile() {
    editorDocument_ = selectedDocument(); const auto doc = context(editorDocument_); profiles_.clear();
    if (!doc) { documentState_ = DocumentState::Open; documentExists_ = false; protectedDocument_ = false; return; }
    profiles_ = doc->profiles(); documentExists_ = doc->status().exists; protectedDocument_ = doc->status().fullEncryption;
    documentState_ = !doc->error().isEmpty() || !files_->status(editorDocument_) ? DocumentState::Error :
        doc->status().metadataAvailable ? DocumentState::Open : doc->status().busy ? DocumentState::Loading : DocumentState::Locked;
    if (documentRootSelected()) doc->select(DocumentRootReference{editorDocument_});
    else doc->select(ProfileReference{editorDocument_, selectedId()});
}
void MainWindow::initializeFiles() {
    files_ = new DocumentService(this);
    editor_->setPasswordHandler([this](PasswordEdit edit) {
        pendingPassword_.emplace(PendingPassword{{editorDocument_, editor_->id()}, std::move(edit), {}});
        if (credentialAttempt_ && credentialAttempt_->document == editorDocument_) cancelCredentials();
    });
    connect(files_, &DocumentService::stateChanged, this, [this](DocumentId id) {
        if (const auto doc = context(id)) if (const auto status = files_->status(id)) doc->updateStatus(*status);
        updateActions();
        if (!unlockQueue_.isEmpty()) QTimer::singleShot(0, this, &MainWindow::processFileUnlocks);
        if (autoSaveQueued_) QTimer::singleShot(0, this, &MainWindow::startAutoSave);
    });
    connect(files_, &DocumentService::locked, this, [this](DocumentId id) {
        if (const auto doc = context(id); doc && !closeApproved_) notification("Locked " + QDir::toNativeSeparators(doc->status().path) + "; saved passwords need the file password again.");
        if (credentialAttempt_ && credentialAttempt_->document == id) cancelCredentials();
        if (pendingPassword_ && pendingPassword_->profile.document == id) {
            pendingPassword_.reset();
            if (editorDocument_ == id) editor_->requirePasswordReentry();
        }
        finishFileUnlock(id, false);
        auto dialog = fileDialogs_.take(id); if (dialog) dialog->reject();
        rebuildFiles();
    });
    ui_->actionNewFile->setEnabled(true); ui_->actionOpenFile->setEnabled(true);
    ui_->actionNewFile->setText("New connection file…");
    ui_->actionUnlockPasswords->setText("Unlock passwords…"); ui_->actionLockPasswords->setText("Lock passwords");
    ui_->actionChangeMaster->setText("Set or change file password…");
    ui_->menuFile->insertAction(ui_->actionSaveAsConnection, ui_->actionSaveFile);
    ui_->menuFile->insertAction(ui_->actionSaveAsConnection, ui_->actionSaveFileAs);
    closeFileAction_ = new QAction("Close connection file", this); closeFileAction_->setObjectName("actionCloseFile");
    ui_->menuFile->insertAction(ui_->actionSaveAsConnection, closeFileAction_);
    connect(closeFileAction_, &QAction::triggered, this, &MainWindow::closeConnectionFile);
    repairIdsAction_ = new QAction("Repair missing connection IDs and save…", this); repairIdsAction_->setObjectName("actionRepairIds");
    ui_->menuTools->insertAction(ui_->actionChangeMaster, repairIdsAction_);
    connect(repairIdsAction_, &QAction::triggered, this, [this] { const auto id = selectedDocument(); guard([this, id] { repairFileIds(id); }); });
    connect(ui_->actionSaveFile, &QAction::triggered, this, &MainWindow::saveConnectionFile);
    connect(ui_->actionNewFile, &QAction::triggered, this, [this] {
        guard([this] {
            const auto path = QFileDialog::getSaveFileName(this, "New connection file", {}, "Connection XML (*.xml)", nullptr, QFileDialog::DontConfirmOverwrite);
            if (!path.isEmpty()) openConnectionFile(path, true);
        });
    });
    connect(ui_->actionOpenFile, &QAction::triggered, this, [this] {
        guard([this] { for (const auto& path : QFileDialog::getOpenFileNames(this, "Open connection files", {}, "Connection XML (*.xml);;All files (*)")) openConnectionFile(path); });
    });
    connect(ui_->actionSaveFileAs, &QAction::triggered, this, [this] {
        const auto id = selectedDocument();
        guard([this, id] {
            if (!selectConnectionFile(id)) return;
            const auto path = QFileDialog::getSaveFileName(this, "Save connection file as", {}, "Connection XML (*.xml)", nullptr, QFileDialog::DontConfirmOverwrite);
            if (!path.isEmpty()) saveConnectionFileAs(path);
        });
    });
    connect(ui_->actionUnlockPasswords, &QAction::triggered, this, [this] { promptFileUnlock(selectedDocument()); });
    connect(ui_->actionLockPasswords, &QAction::triggered, this, [this] { files_->lock(selectedDocument()); });
    connect(ui_->actionChangeMaster, &QAction::triggered, this, [this] { const auto id = selectedDocument(); guard([this, id] { changeFileMaster(id); }); });
    connect(ui_->actionDifferentCredentials, &QAction::triggered, this, &MainWindow::connectDifferent);
    connect(ui_->connectionTree, &ConnectionTree::scopedMoveRequested, this, [this](ProfileReference source, ConnectionDestination destination, QString before) {
        guard([this, source, destination, before] {
            if (!ui_->connectionTree->validMove(source, destination, before)) return;
            const auto doc = context(source.document); if (!doc || !doc->canWrite()) return;
            selectConnectionProfile(source);
            const auto* parent = std::get_if<ProfileReference>(&destination);
            const QJsonObject request{{"id", source.profile}, {"parent", parent ? QJsonValue(parent->profile) : QJsonValue(QJsonValue::Null)},
                {"before", before.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(before)}};
            // A manual placement replaces any view-only A–Z/Z–A sort.
            if (!before.isEmpty() || doc->profiles().value(source.profile).value("parent").toString() == (parent ? parent->profile : QString())) treeOrder_.reset();
            writeFileProfile(rm::ProfileChange::Move, QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact)), {});
        });
    });
    openConnectionFile(dataPath_, !QFileInfo::exists(dataPath_));
}
DocumentId MainWindow::openConnectionFile(const QString& path, bool createNew) {
    if (!files_ || closeApproved_) return {};
    const auto id = files_->open(path, [this](DocumentReply reply) {
        if (reply.alreadyOpen) {
            documents_.remove(reply.status.id); documentOrder_.removeAll(reply.status.id); rebuildFiles();
            selectConnectionFile(*reply.alreadyOpen); return;
        }
        const auto id = reply.status.id;
        const bool prompt = reply.result.ok && !reply.status.unlocked && !reply.canceled;
        if (reply.result.ok) notification("Opened " + QDir::toNativeSeparators(reply.status.path) + (reply.status.unlocked ? "" : " (locked; enter the file password to use saved passwords)"));
        fileReply(std::move(reply)); emit documentReady();
        if (prompt) QTimer::singleShot(0, this, [this, id] { promptFileUnlock(id); });
    }, createNew);
    auto doc = std::make_shared<DocumentContext>(id); if (const auto status = files_->status(id)) doc->updateStatus(*status);
    documents_.insert(id, doc); documentOrder_.append(id); rebuildFiles();
    if (!editor_->dirty() && !autoSaveInFlight_) selectConnectionFile(id);
    return id;
}
void MainWindow::rebuildFiles() {
    const QSignalBlocker blocker(ui_->connectionTree); const auto selected = selectedDocument(); const auto profile = selectedId();
    const auto scroll = ui_->connectionTree->verticalScrollBar()->value();
    for (QTreeWidgetItemIterator it(ui_->connectionTree); *it; ++it) if ((*it)->type() == DocumentItem) {
        if (const auto doc = context((*it)->data(0, Qt::UserRole).value<DocumentRootReference>().document)) doc->setRootExpanded((*it)->isExpanded());
    } else if ((*it)->type() == ProfileItem) {
        const auto ref = (*it)->data(0, Qt::UserRole).value<ProfileReference>(); if (const auto doc = context(ref.document)) {
            if ((*it)->isExpanded()) doc->expanded().insert(ref.profile); else doc->expanded().remove(ref.profile);
        }
    }
    QHash<DocumentId, QTreeWidgetItem*> roots;
    for (int index = ui_->connectionTree->topLevelItemCount() - 1; index >= 0; --index) {
        auto* root = ui_->connectionTree->topLevelItem(index);
        const auto id = root->data(0, Qt::UserRole).value<DocumentRootReference>().document;
        if (context(id)) roots.insert(id, root);
        else { treeGenerations_.remove(id); delete ui_->connectionTree->takeTopLevelItem(index); }
    }
    for (auto id : documentOrder_) {
        const auto doc = context(id); const auto& status = doc->status();
        const auto suffix = status.busy && !status.metadataAvailable ? " — loading" : !doc->error().isEmpty() || !files_->status(id) ? " — unavailable" :
            !status.unlocked ? " — locked" : status.repairRequired ? " — read-only: missing IDs" : status.passwordProtected == true ? " — password protected" : status.fullEncryption ? " — full encryption, default key" : "";
        auto* root = roots.value(id);
        if (!root) {
            root = new QTreeWidgetItem(ui_->connectionTree, DocumentItem);
            root->setData(0, Qt::UserRole, QVariant::fromValue(DocumentRootReference{id}));
            root->setIcon(0, QIcon(":/original/ASPWebSite_16x.png")); root->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        }
        root->setText(0, "Connections — " + QFileInfo(status.path).fileName() + suffix); root->setToolTip(0, status.path);
        root->setExpanded(doc->rootExpanded());
        if (treeGenerations_.contains(id) && treeGenerations_.value(id) == doc->treeGeneration()) continue;
        // Replace only the owning subtree. Other roots keep their item identities.
        qDeleteAll(root->takeChildren());
        QHash<QString, QTreeWidgetItem*> items;
        for (auto it = doc->profiles().cbegin(); it != doc->profiles().cend(); ++it) {
            auto* item = new QTreeWidgetItem({it.value()["name"].toString()}, ProfileItem);
            item->setData(0, Qt::UserRole, QVariant::fromValue(ProfileReference{id, it.key()}));
            item->setIcon(0, ConnectionIcons::profile(it.value()));
            items.insert(it.key(), item);
        }
        for (const auto& profile : doc->order()) {
            const auto parent = doc->profiles().value(profile)["parent"].toString();
            (parent.isEmpty() ? root : items.value(parent))->addChild(items.value(profile));
            items.value(profile)->setExpanded(doc->expanded().contains(profile));
        }
        for (auto* pane : sessionPages()) if (pane->document() == id && doc->profiles().contains(pane->profile())) {
            if (auto* tabs = ownerOf(pane)) tabs->setTabIcon(tabs->indexOf(pane), ConnectionIcons::profile(doc->profiles().value(pane->profile())));
        }
        treeGenerations_.insert(id, doc->treeGeneration());
    }
    QTreeWidgetItem* current = nullptr;
    for (QTreeWidgetItemIterator it(ui_->connectionTree); *it; ++it) {
        const auto value = (*it)->data(0, Qt::UserRole);
        if ((*it)->type() == DocumentItem && profile.isEmpty() && value.value<DocumentRootReference>().document == selected) current = *it;
        else if ((*it)->type() == ProfileItem && value.value<ProfileReference>() == ProfileReference{selected, profile}) current = *it;
    }
    if (!current && ui_->connectionTree->topLevelItemCount()) current = ui_->connectionTree->topLevelItem(0);
    ui_->connectionTree->setCurrentItem(current);
    if (credentialAttempt_ && !validAttempt(credentialAttempt_)) cancelCredentials();
    if (!editor_->dirty() && !autoSaveInFlight_) properties();
    if (treeOrder_) ui_->connectionTree->sortItems(0, *treeOrder_);
    filterConnections(); ui_->connectionTree->verticalScrollBar()->setValue(scroll); updateActions();
}
void MainWindow::fileWarning(DocumentId id, std::function<void()> next) {
    const auto doc = context(id); if (!doc) return;
    if (doc->status().warning.isEmpty()) { if (next) guard(std::move(next)); return; }
    auto* dialog = new QMessageBox(QMessageBox::Warning, "Saved — durability warning", doc->status().warning, QMessageBox::Ok, this);
    dialog->setObjectName("durabilityWarning"); dialog->setTextFormat(Qt::PlainText); dialog->setAttribute(Qt::WA_DeleteOnClose);
    // This warning gates its file only. Other roots remain usable.
    connect(dialog, &QDialog::finished, this, [this, id, next = std::move(next)](int answer) mutable {
        if (answer != QMessageBox::Ok) return;
        files_->acknowledgeWarning(id); if (const auto doc = context(id)) doc->acknowledgeWarning();
        updateActions(); if (next) guard(std::move(next));
        if (closeRequested_) QTimer::singleShot(0, this, [this] { close(); });
    }); dialog->setWindowModality(Qt::NonModal); dialog->show();
}
void MainWindow::fileReply(DocumentReply reply, QString profile, std::function<void()> next, bool automatic) {
    const auto id = reply.status.id; const auto doc = context(id); if (!doc) return;
    const auto result = doc->adopt(reply); const bool owningEditor = editorDocument_ == id;
    if (reply.committed && pendingPassword_ && pendingPassword_->profile.document == id) pendingPassword_.reset();
    if (reply.committed && owningEditor && result.ok && editor_->passwordPending()) editor_->finishPasswordEdit();
    if (reply.committed && owningEditor && !editor_->defaultsMode()) {
        profiles_ = doc->profiles();
        if (editor_->documentRoot()) editor_->loadDocument(doc->status().passwordProtected, doc->status().fullEncryption, documentHelp(), doc->canWrite());
        else editor_->adopt(profiles_.value(profile.isEmpty() ? editor_->id() : profile), profiles_);
    }
    rebuildFiles();
    if (!result.ok) {
        if (automatic) autoSaveFailed(result.error);
        else { finishTransition(); fail(result.error); }
        return;
    }
    if (reply.committed) {
        lastError_.clear(); notification("Saved " + QDir::toNativeSeparators(doc->status().path) + (profile.isEmpty() ? QString() : " (" + doc->profiles().value(profile).value("name").toString() + ")"));
        if (!automatic && !profile.isEmpty() && owningEditor) selectConnectionProfile({id, profile});
    }
    if (automatic && !doc->status().warning.isEmpty()) {
        auto pending = std::move(pendingTransition_); pendingTransition_ = {};
        autoSaveInFlight_ = false; autoSaveQueued_ = false; autoSaveError_ = false;
        finishTransition(); properties(); updateActions();
        fileWarning(id, [this, pending = std::move(pending), next = std::move(next), canceled = reply.canceled] {
            if (canceled || closeApproved_) return;
            if (pending) pending();
            if (next) next();
        });
    } else if (automatic) {
        finishTransition(); autoSaveFinished(reply.canceled); if (!reply.canceled && next) next();
    } else {
        finishTransition(); fileWarning(id, reply.canceled ? std::function<void()>() : std::move(next));
    }
    if (closeRequested_) QTimer::singleShot(0, this, [this] { close(); });
}
void MainWindow::writeFileProfile(rm::ProfileChange change, const QString& input, std::function<void()> next, bool inheritPassword) {
    const auto id = editorDocument_; const auto doc = context(id); const bool automatic = autoSaveInFlight_;
    if (!doc || !doc->canWrite()) { if (automatic) autoSaveFailed("Unlock this connection file before saving."); else fail("Unlock this connection file before saving."); return; }
    try {
        const auto candidate = doc->prepareProfile(change, input.toUtf8()); updateActions();
        auto complete = [this, profile = candidate.profile, next = std::move(next), automatic](DocumentReply reply) mutable {
            fileReply(std::move(reply), profile, std::move(next), automatic);
        };
        if (inheritPassword)
            files_->editPassword({id, candidate.profile}, candidate.projection, {PasswordEdit::Kind::Inheritance, {}, true}, {}, {}, candidate.reservation, std::move(complete));
        else files_->save(id, candidate.projection, candidate.reservation, std::move(complete));
    } catch (const std::exception&) { if (automatic) autoSaveFailed("Cannot prepare this file's profile edit."); else fail("Cannot prepare this file's profile edit."); }
}
void MainWindow::unlockConnectionFile(DocumentId id, Secret master) {
    if (!context(id) || !files_) return;
    fileUnlocks_.remove(id); unlockQueue_.removeAll(id);
    if (unlockingFile_ == id) unlockingFile_ = {};
    auto dialog = fileDialogs_.take(id); if (dialog) dialog->reject();
    files_->unlock(id, std::move(master), [this, id](DocumentReply reply) { fileReply(std::move(reply), {}, [this, id] { reviewFilePasswords(id); }); });
}
void MainWindow::promptFileUnlock(DocumentId id, std::function<void()> next) {
    const auto doc = context(id); if (!doc || closeApproved_ || !doc->status().warning.isEmpty()) return;
    if (doc->status().unlocked) { reviewFilePasswords(id, std::move(next)); return; }
    auto request = fileUnlocks_.value(id);
    if (!request) {
        request = std::make_shared<FileUnlock>(); request->generation = doc->status().generation; request->revision = doc->status().revision;
        fileUnlocks_.insert(id, request); unlockQueue_.append(id);
    }
    if (next) request->continuations.push_back(std::move(next));
    if (fileDialogs_.value(id)) fileDialogs_.value(id)->raise();
    QTimer::singleShot(0, this, &MainWindow::processFileUnlocks);
}
void MainWindow::finishFileUnlock(DocumentId id, bool unlocked) {
    const auto request = fileUnlocks_.take(id); if (!request) return;
    unlockQueue_.removeAll(id); if (unlockingFile_ == id) unlockingFile_ = {};
    if (!unlocked) {
        if (credentialAttempt_ && credentialAttempt_->document == id) cancelCredentials();
    } else reviewFilePasswords(id, [request] { for (const auto& next : request->continuations) next(); });
    QTimer::singleShot(0, this, &MainWindow::processFileUnlocks);
}
void MainWindow::processFileUnlocks() {
    if (closeApproved_ || unlockingFile_.valid() || unlockQueue_.isEmpty()) return;
    for (auto dialog : fileDialogs_) if (dialog) return;
    const auto id = unlockQueue_.first(); const auto request = fileUnlocks_.value(id); const auto doc = context(id);
    if (!request || !doc || doc->status().generation != request->generation || doc->status().revision != request->revision) { finishFileUnlock(id, false); return; }
    if (doc->status().busy) return;
    if (doc->status().unlocked) { finishFileUnlock(id, true); return; }
    if (!doc->status().warning.isEmpty()) { finishFileUnlock(id, false); return; }
    unlockingFile_ = id;
    const auto current = [this, id, request] {
        const auto doc = context(id);
        return !closeApproved_ && fileUnlocks_.value(id) == request && doc &&
            doc->status().generation == request->generation && doc->status().revision == request->revision;
    };
    const auto submit = [this, id, request, current](Secret master) {
        ++request->submissions;
        files_->unlock(id, std::move(master), [this, id, request, current](DocumentReply reply) {
            if (!current()) { finishFileUnlock(id, false); return; }
            if (reply.result.ok && !reply.canceled && reply.status.unlocked) {
                notification("Unlocked " + QDir::toNativeSeparators(reply.status.path) + "; it stays unlocked until you lock or close it.");
                fileReply(std::move(reply), {}, [this, id] { finishFileUnlock(id, true); }); return;
            }
            request->error = reply.result.error;
            if (reply.canceled || request->submissions >= 3 || context(id)->status().passwordProtected == false) {
                fileReply(std::move(reply)); finishFileUnlock(id, false); return;
            }
            unlockingFile_ = {}; QTimer::singleShot(0, this, &MainWindow::processFileUnlocks);
        });
    };
    if (doc->status().passwordProtected == false) { submit(Secret(QByteArray("mR3m"))); return; }
    auto* dialog = FilePasswordDialogs::passphrase(this, FilePasswordDialogs::Mode::Unlock,
        [this, id, current, submit](std::optional<Secret> master) {
            if (!current()) return;
            fileDialogs_.remove(id);
            if (!master) { finishFileUnlock(id, false); return; }
            submit(std::move(*master));
        });
    dialog->setObjectName("filePassword"); dialog->setWindowTitle("Unlock connection file — " + QFileInfo(doc->status().path).fileName());
    dialog->findChild<QLabel*>("hintLabel")->setText(doc->status().path + "\n\nEnter this file's exact master password (single-byte/Latin-1 characters). Cancel leaves this file locked and unchanged. Opened metadata and sessions remain visible." +
        QString("\nAttempt %1 of 3.").arg(request->submissions + 1));
    dialog->findChild<QLabel*>("errorLabel")->setText(request->error);
    connect(dialog, &QObject::destroyed, this, [this] { QTimer::singleShot(0, this, &MainWindow::processFileUnlocks); });
    fileDialogs_.insert(id, dialog);
}
void MainWindow::changeFileMaster(DocumentId id, std::function<void()> next) {
    const auto doc = context(id); if (!doc || fileDialogs_.value(id)) return;
    if (!doc->status().unlocked) { promptFileUnlock(id, [this, id, next = std::move(next)] { changeFileMaster(id, next); }); return; }
    if (!doc->canWrite()) return;
    const auto generation = doc->status().generation; const auto revision = doc->status().revision;
    promptNewFileMaster(id, [this, id, generation, revision, next = std::move(next)](std::optional<Secret> master) mutable {
        const auto doc = context(id); if (!master || !doc || closeApproved_) return;
        if (doc->status().generation != generation || doc->status().revision != revision) { fail("The owning file changed or was locked. No password change was saved."); return; }
        try {
            const auto candidate = doc->prepareDocument();
            files_->changeMaster(id, std::move(*master), candidate.reservation, [this, next = std::move(next)](DocumentReply reply) mutable { fileReply(std::move(reply), {}, std::move(next)); });
        } catch (const std::exception&) { fail("Cannot reserve this file's password change."); }
    });
}
void MainWindow::promptNewFileMaster(DocumentId id, std::function<void(std::optional<Secret>)> complete) {
    auto* dialog = FilePasswordDialogs::passphrase(this, FilePasswordDialogs::Mode::ChangeMaster,
        [this, id, complete = std::move(complete)](std::optional<Secret> master) mutable {
            fileDialogs_.remove(id);
            if (master && !ConnectionCrypto::validNewMaster(*master)) { master.reset(); fail("Use 1–4096 compatible single-byte characters, not whitespace-only or mR3m. File unchanged."); }
            complete(std::move(master));
        });
    dialog->setObjectName("filePassword"); dialog->setWindowTitle("Set or change file password");
    const auto doc = context(id);
    dialog->findChild<QLabel*>("hintLabel")->setText((doc ? doc->status().path + "\n\n" : QString()) +
        "Use 1–4096 compatible single-byte/Latin-1 characters, not whitespace-only or mR3m. A long password is recommended. Enter the same exact password twice. All known secrets are re-encrypted in this XML file. Lost passwords cannot be recovered. Old copies still use their old passwords.");
    connect(dialog, &QObject::destroyed, this, [this] { QTimer::singleShot(0, this, &MainWindow::processFileUnlocks); });
    fileDialogs_.insert(id, dialog);
}
void MainWindow::passwordProtectFile(bool enabled) {
    const auto id = editorDocument_; const auto doc = context(id); if (!doc) return;
    if (doc->status().passwordProtected == enabled) { revertEditor(); autoSaveFinished(false); return; }
    const auto generation = doc->status().generation; const auto revision = doc->status().revision;
    const auto apply = [this, id, generation, revision](std::optional<Secret> master, bool reset) {
        const auto doc = context(id);
        if ((!reset && !master) || closeApproved_) { revertEditor(); autoSaveFinished(true); return; }
        if (!doc || !doc->canWrite() || doc->status().generation != generation || doc->status().revision != revision) {
            autoSaveFailed("The owning file changed or was locked. No password protection change was saved."); return;
        }
        try {
            const auto candidate = doc->prepareDocument();
            const auto complete = [this](DocumentReply reply) { fileReply(std::move(reply), {}, {}, true); };
            if (reset) files_->resetMaster(id, candidate.reservation, complete);
            else files_->changeMaster(id, std::move(*master), candidate.reservation, complete);
        } catch (const std::exception&) { autoSaveFailed("Cannot reserve this file's password protection change."); }
    };
    if (enabled) { promptNewFileMaster(id, [apply](std::optional<Secret> master) { apply(std::move(master), false); }); return; }
    auto* question = new QMessageBox(QMessageBox::Warning, "Remove custom file password",
        "Re-encrypt all saved passwords with mRemoteNG's publicly known mR3m default key? Anyone with this XML file can recover them. Saved passwords and Encrypt entire file are retained. Old copies still use their old passwords.",
        QMessageBox::Yes | QMessageBox::Cancel, this);
    question->setObjectName("confirmRemoveMaster"); question->setTextFormat(Qt::PlainText); question->setDefaultButton(QMessageBox::Cancel);
    question->setAttribute(Qt::WA_DeleteOnClose); fileDialogs_.insert(id, question);
    connect(question, &QDialog::finished, this, [this, id, apply](int answer) {
        fileDialogs_.remove(id);
        if (answer != QMessageBox::Yes) { revertEditor(); autoSaveFinished(true); return; }
        apply({}, true);
    }); question->open();
}
void MainWindow::protectFile(bool enabled) {
    const auto id = editorDocument_; const auto doc = context(id); if (!doc) return;
    if (enabled == doc->status().fullEncryption) { revertEditor(); autoSaveFinished(false); return; }
    const auto generation = doc->status().generation; const auto revision = doc->status().revision;
    auto* question = new QMessageBox(QMessageBox::Warning, enabled ? "Encrypt connection metadata" : "Make connection metadata readable",
        enabled ? "Encrypt the inner connection XML with this file's current key? Root attributes remain public. Password Protect is unchanged. Without a custom password, the public mR3m key provides obfuscation only."
                : "Make names, hosts and accounts readable? Password attributes stay encrypted. Password Protect is unchanged. Opened metadata and sessions remain visible when locked.", QMessageBox::Yes | QMessageBox::Cancel, this);
    question->setObjectName("confirmProtection"); question->setTextFormat(Qt::PlainText); question->setDefaultButton(QMessageBox::Cancel);
    question->setAttribute(Qt::WA_DeleteOnClose); fileDialogs_.insert(id, question);
    connect(question, &QDialog::finished, this, [this, id, enabled, generation, revision](int answer) {
        fileDialogs_.remove(id);
        if (answer != QMessageBox::Yes || closeApproved_) { revertEditor(); autoSaveFinished(true); return; }
        const auto doc = context(id);
        if (!doc || !doc->canWrite() || doc->status().generation != generation || doc->status().revision != revision) {
            autoSaveFailed("The owning file changed or was locked. No full encryption change was saved."); return;
        }
        try { const auto candidate = doc->prepareDocument(); files_->protect(id, enabled, candidate.reservation, [this](DocumentReply reply) { fileReply(std::move(reply), {}, {}, true); }); }
        catch (const std::exception&) { autoSaveFailed("Cannot reserve this file's full encryption change."); }
    }); question->open();
}
void MainWindow::saveConnectionFile() {
    const auto id = selectedDocument();
    guard([this, id] {
        const auto doc = context(id); if (!doc) return;
        if (!doc->status().unlocked) { promptFileUnlock(id, [this, id] { if (selectConnectionFile(id)) saveConnectionFile(); }); return; }
        try { const auto candidate = doc->prepareDocument(); files_->save(id, candidate.projection, candidate.reservation, [this](DocumentReply reply) { fileReply(std::move(reply)); }); }
        catch (const std::exception&) { fail("This file cannot be saved while busy or awaiting recovery."); }
    });
}
void MainWindow::saveConnectionFileAs(const QString& path, bool overwriteConfirmed) {
    const auto id = selectedDocument();
    guard([this, id, path, overwriteConfirmed] {
        const auto doc = context(id); if (!doc) return;
        if (!doc->status().unlocked) { promptFileUnlock(id, [this, id, path, overwriteConfirmed] { if (selectConnectionFile(id)) saveConnectionFileAs(path, overwriteConfirmed); }); return; }
        files_->inspectDestination(id, path, [this, id, overwriteConfirmed](DocumentReply reply) {
            const auto doc = context(id); if (!doc) return;
            doc->adopt(reply); updateActions();
            if (!reply.result.ok || !reply.destination) { fail(reply.result.error); return; }
            const auto ticket = reply.destination->ticket;
            auto save = [this, id, ticket] {
                const auto doc = context(id); if (!doc) return;
                try { const auto candidate = doc->prepareDocument(); files_->saveAs(id, ticket, true, candidate.reservation, [this](DocumentReply reply) { fileReply(std::move(reply)); }); }
                catch (const std::exception&) { fail("This file cannot be saved while busy or awaiting recovery."); }
            };
            if (!reply.destination->exists || overwriteConfirmed) { save(); return; }
            auto* question = new QMessageBox(QMessageBox::Warning, "Replace inspected connection file", "Replace " + reply.destination->path + "? The destination will be checked again before replacement.", QMessageBox::Yes | QMessageBox::Cancel, this);
            question->setObjectName("confirmFileOverwrite"); question->setTextFormat(Qt::PlainText); question->setDefaultButton(QMessageBox::Cancel); question->setAttribute(Qt::WA_DeleteOnClose);
            connect(question, &QDialog::finished, this, [save](int answer) { if (answer == QMessageBox::Yes) save(); }); question->open();
        });
    });
}
void MainWindow::closeConnectionFile() {
    const auto id = selectedDocument();
    guard([this, id] {
        const auto doc = context(id); if (!doc) return;
        if (!doc->canClose()) { fail("Close this file's saved tabs, including disconnected tabs, and settle its saves and warnings before closing it."); return; }
        auto remove = [this, id] {
            finishFileUnlock(id, false);
            auto dialog = fileDialogs_.take(id); if (dialog) dialog->reject();
            documents_.remove(id); documentOrder_.removeAll(id); rebuildFiles();
        };
        if (!files_->status(id)) { remove(); return; }
        files_->close(id, [this, remove](DocumentReply reply) { if (reply.closed) remove(); else fileReply(std::move(reply)); });
    });
}
void MainWindow::saveTemporaryToFile(QPointer<SessionPane> pane) {
    if (!pane || !pane->profile().isEmpty()) return;
    const auto target = pane->target();
    auto* dialog = new QDialog(this); dialog->setObjectName("saveAsConnection"); dialog->setWindowTitle("Save as connection");
    dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowModality(Qt::WindowModal);
    auto* layout = new QFormLayout(dialog);
    auto* name = new QLineEdit(target.host, dialog); name->setObjectName("connectionName");
    auto* file = new QComboBox(dialog); file->setObjectName("connectionFile");
    auto* folder = new QComboBox(dialog); folder->setObjectName("connectionParent");
    for (auto id : documentOrder_) if (const auto doc = context(id); doc->canWrite()) file->addItem(doc->status().path, QVariant::fromValue(id));
    if (!file->count()) { delete dialog; fail("Open and unlock a target connection file first."); return; }
    const auto index = file->findData(QVariant::fromValue(selectedDocument())); if (index >= 0) file->setCurrentIndex(index);
    auto folders = [this, file, folder] {
        folder->clear(); folder->addItem("Connections root", QVariant::fromValue(DocumentRootReference{file->currentData().value<DocumentId>()}));
        if (const auto doc = context(file->currentData().value<DocumentId>())) for (auto it = doc->profiles().cbegin(); it != doc->profiles().cend(); ++it)
            if (it.value()["kind"] == "folder") folder->addItem(it.value()["name"].toString(), QVariant::fromValue(ProfileReference{doc->id(), it.key()}));
    };
    connect(file, &QComboBox::currentIndexChanged, dialog, folders); folders();
    layout->addRow("Name", name); layout->addRow("Connection file", file); layout->addRow("Parent folder", folder);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog); layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::finished, this, [this, pane, target, name, file, folder](int answer) {
        if (answer != QDialog::Accepted || !pane || closeApproved_) return;
        const auto id = file->currentData().value<DocumentId>(); const auto doc = context(id);
        if (!doc || !doc->canWrite()) { fail("Target connection file is closed, locked, busy or awaiting recovery."); return; }
        const auto value = folder->currentData(); QString parent;
        if (value.canConvert<ProfileReference>()) {
            const auto ref = value.value<ProfileReference>();
            if (ref.document != id || doc->profiles().value(ref.profile)["kind"] != "folder") { fail("Target folder is no longer available."); return; }
            parent = ref.profile;
        } else if (value.value<DocumentRootReference>().document != id) { fail("Target root is no longer available."); return; }
        const QJsonObject request{{"name", name->text()}, {"kind", "connection"}, {"parent", parent.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(parent)}, {"overrides", target.overrides()}};
        const auto input = QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact));
        guard([this, id, parent, input] { if (parent.isEmpty() ? selectConnectionFile(id) : selectConnectionProfile({id, parent})) writeFileProfile(rm::ProfileChange::Create, input, {}); });
    }); dialog->open(); name->selectAll(); name->setFocus();
}
void MainWindow::updateFileActions() {
    if (!files_ || !closeFileAction_) return;
    const auto id = selectedDocument(); const auto doc = context(id);
    const bool ready = doc && doc->canWrite() && !autoSaveError_ && !transitionPending_;
    QHash<DocumentId, DocumentTreeState> trees; bool anyWritable = false;
    for (auto id : documentOrder_) { const auto doc = context(id); anyWritable = anyWritable || doc->canWrite(); trees.insert(id, {doc->profiles(), doc->treeGeneration(), doc->canWrite() && !autoSaveError_ && !transitionPending_, doc->order()}); }
    ui_->connectionTree->setCommittedDocuments(trees); ui_->connectionTree->setMovesEnabled(anyWritable && !closeApproved_ && !transitionPending_ && !autoSaveError_);
    const bool configEditable = !transitionPending_ && !autoSaveInFlight_ && (editor_->defaultsMode() ? preferencesReady_ : doc && doc->canWrite());
    ui_->configTree->setEnabled(configEditable && (editor_->defaultsMode() || documentRootSelected() || !selectedId().isEmpty()));
    ui_->actionConnectionIcon->setEnabled(configEditable && !editor_->inheritance() && (editor_->defaultsMode() || !selectedId().isEmpty()));
    if (!editor_->defaultsMode()) editor_->setPasswordState(doc ? doc->status().passwords.value(editor_->id()) : PasswordState{},
        doc && doc->canWrite() && !transitionPending_);
    ui_->connectionTree->setEnabled(!transitionPending_);
    ui_->actionAddFolder->setEnabled(ready); ui_->actionAddConnection->setEnabled(ready);
    ui_->actionTreeRename->setEnabled(ready && !selectedId().isEmpty()); ui_->actionTreeDelete->setEnabled(ready && !selectedId().isEmpty());
    ui_->actionTreeDuplicate->setEnabled(ready && !selectedId().isEmpty());
    ui_->actionImportFile->setEnabled(ready); ui_->actionImportPutty->setEnabled(ready);
    ui_->actionExport->setEnabled(doc && doc->status().metadataAvailable && !transitionPending_);
    ui_->actionMoveUp->setEnabled(ready && siblingMove(-1).has_value()); ui_->actionMoveDown->setEnabled(ready && siblingMove(1).has_value());
    const bool connectable = !transitionPending_ && committedTarget(selectedId()).has_value();
    for (auto* action : {ui_->actionConnectFullscreen, ui_->actionNoCredentials, ui_->actionChoosePanel}) action->setEnabled(connectable);
    ui_->actionConsoleSession->setEnabled(connectable && committedTarget(selectedId())->protocol == "rdp");
    ui_->actionHostStatus->setEnabled(connectable); ui_->actionReconnectAll->setEnabled(!transitionPending_ && !sessionPages().isEmpty());
    ui_->actionSaveFile->setEnabled(ready); ui_->actionSaveFileAs->setEnabled(ready); closeFileAction_->setEnabled(doc && !doc->status().busy);
    repairIdsAction_->setEnabled(doc && doc->canRepairIds() && !transitionPending_ && !autoSaveError_);
    bool reviewable = false;
    if (doc && doc->canWrite() && doc->status().reviewRequired)
        for (auto it = doc->profiles().cbegin(); it != doc->profiles().cend(); ++it)
            reviewable = reviewable || (it.value()["kind"] == "connection" && doc->status().passwords.value(it.key()).present);
    ui_->actionUnlockPasswords->setText(reviewable ? "Review imported passwords…" : "Unlock passwords…");
    ui_->actionUnlockPasswords->setEnabled(doc && !doc->status().busy && (!doc->status().unlocked || reviewable) && doc->status().warning.isEmpty());
    ui_->actionLockPasswords->setEnabled(doc && (doc->status().unlocked || doc->status().compatibilityMaster || doc->status().busy)); ui_->actionChangeMaster->setEnabled(doc && !doc->status().busy && !transitionPending_ && !autoSaveError_ && doc->status().warning.isEmpty());
    ui_->actionDifferentCredentials->setEnabled(ui_->actionTreeConnect->isEnabled() && profiles_.value(selectedId()).value("kind") == "connection");
    ui_->actionSaveAsConnection->setEnabled(currentSessions() && qobject_cast<SessionPane*>(currentSessions()->currentWidget()) &&
        qobject_cast<SessionPane*>(currentSessions()->currentWidget())->profile().isEmpty() && anyWritable && !transitionPending_);
}
void MainWindow::beginFileCredentials(DocumentId document, const QString& profile, SessionTarget target, QTabWidget* owner, SessionPane* replacement, bool promptOnly, const QString& folder) {
    auto attempt = std::make_shared<CredentialAttempt>(); attempt->profile = profile; attempt->document = document; attempt->target = target; attempt->folder = folder;
    attempt->owner = owner; attempt->page = replacement; attempt->replacing = replacement != nullptr; attempt->token = ++credentialToken_;
    if (document.valid()) {
        const auto doc = context(document); if (!doc) return;
        attempt->generation = doc->status().generation; attempt->revision = doc->status().revision;
        const auto stored = attemptTarget(*doc, profile, folder);
        attempt->console = stored && target.protocol == "rdp" && target.options.consoleSession && !stored->options.consoleSession;
    }
    credentialAttempt_ = attempt; credentialPage_ = replacement; credentialOwner_ = owner;
    if (!document.valid() || promptOnly || (target.protocol == "ssh" && target.sshAuth == "private_key")) { promptCredentials(attempt); return; }
    const auto doc = context(document);
    if (!doc->status().unlocked || doc->status().reviewRequired) {
        promptFileUnlock(document, [this, attempt] {
            if (!validAttempt(attempt)) return;
            const auto doc = context(attempt->document); if (!doc || !doc->status().unlocked) return;
            const auto effective = doc->profiles().value(attempt->profile)["effective"].toObject();
            const auto target = expectedTarget(*attempt);
            if (!target || !(*target == attempt->target) || !effective["unsupported"].toArray().isEmpty()) { cancelCredentials(); return; }
            attempt->generation = doc->status().generation; attempt->revision = doc->status().revision;
            lookupFileCredentials(attempt);
        }); return;
    }
    lookupFileCredentials(attempt);
}
void MainWindow::lookupFileCredentials(Attempt attempt) {
    if (!validAttempt(attempt)) return;
    files_->lookup({attempt->document, attempt->profile}, attempt->target, [this, attempt](DocumentReply reply) {
        if (!validAttempt(attempt)) return;
        if (!reply.result.ok || reply.canceled || reply.status.generation != attempt->generation || reply.status.revision != attempt->revision) { cancelCredentials(); fail("Owning file changed, locked, or canceled the password request."); return; }
        auto use = [this, attempt](Secret password) {
            if (attempt->target.protocol == "vnc") {
                // The reviewed saved password is sent only if the server requires one.
                try { attempt->vncAuthentication = vncPasswordAuthentication(std::move(password)); }
                catch (const std::exception&) {
                    fail("The saved VNC password is not 1–8 printable ASCII characters; you will be asked if the server requires a password.");
                    promptCredentials(attempt); return;
                }
                finishCredentials(attempt); return;
            }
            // Bindings authenticate the server endpoint/account, not a gateway. With
            // "same" gateway credentials FreeRDP would also send the saved password
            // to the gateway host, so ask for this session instead (V106).
            if (attempt->target.protocol == "rdp" && attempt->target.options.gatewayEnabled() && attempt->target.options.gatewayCredentials == "same") {
                promptCredentials(attempt); return;
            }
            attempt->credentials.emplace(); attempt->credentials->username = attempt->target.username; attempt->credentials->domain = attempt->target.domain;
            attempt->credentials->password = password.takeBytes(); finishCredentials(attempt);
        };
        if (reply.password) { use(std::move(*reply.password)); return; }
        if (attempt->folder.isEmpty()) { promptCredentials(attempt); return; }
        // Folder connect: a connection without its own saved password uses the folder's.
        files_->lookupFolder({attempt->document, attempt->folder}, [this, attempt, use](DocumentReply reply) {
            if (!validAttempt(attempt)) return;
            if (!reply.result.ok || reply.canceled || reply.status.generation != attempt->generation || reply.status.revision != attempt->revision) { cancelCredentials(); fail("Owning file changed, locked, or canceled the password request."); return; }
            if (reply.password) use(std::move(*reply.password)); else promptCredentials(attempt);
        });
    });
}
std::optional<SessionTarget> MainWindow::expectedTarget(const CredentialAttempt& attempt) const {
    const auto doc = context(attempt.document); if (!doc) return std::nullopt;
    auto target = attemptTarget(*doc, attempt.profile, attempt.folder);
    if (target && attempt.console) target->options.consoleSession = true;
    return target;
}
std::optional<SessionTarget> MainWindow::attemptTarget(const DocumentContext& doc, const QString& profile, const QString& folder) const {
    auto target = sessionTarget(doc.profiles().value(profile).value("effective").toObject());
    if (!folder.isEmpty() && target.username.isEmpty()) {
        const auto source = doc.profiles().value(folder).value("effective").toObject();
        target.username = source.value("username").toString();
        if (target.protocol == "rdp" && target.domain.isEmpty()) target.domain = source.value("domain").toString();
    }
    return normalizeSessionTarget(target);
}
