#include "MainWindow.h"
#include "ProfileEditor.h"
#include <QDialog>
#include <QJsonDocument>
#include <QJsonArray>
#include <QMessageBox>
#include <QTimer>

namespace {
std::optional<SessionTarget> passwordAccount(const QJsonObject& effective) {
    if (!effective["unsupported"].toArray().isEmpty()) return std::nullopt;
    const auto target = sessionTarget(effective);
    if (target.protocol == "ssh") return target.sshAuth == "password" ? normalizeSshTarget(target) : std::nullopt;
    if (target.protocol == "vnc") { auto account = target; account.viewOnly = false; return normalizeVncTarget(account); }
    return normalizeRdpTarget(target);
}
QString accountLabel(const QString& name, const SessionTarget& target) {
    if (target.protocol == "vnc") return name + " — VNC — " + target.endpoint();
    return name + " — " + (target.protocol == "ssh" ? "SSH" : "RDP") + " — " + target.endpoint() + " — " +
        (target.domain.isEmpty() ? target.username : target.domain + "\\" + target.username);
}
}

void MainWindow::repairFileIds(DocumentId id) {
    const auto doc = context(id); if (!doc || !doc->canRepairIds() || closeApproved_) return;
    if (fileDialogs_.value(id)) { fileDialogs_.value(id)->raise(); return; }
    const auto generation = doc->status().generation; const auto revision = doc->status().revision;
    QString text = "This file has connections without IDs and is read-only. Save the currently displayed temporary UUIDs as permanent native IDs in one replacement? Names, accounts, hierarchy and unknown settings are retained. Cancel leaves the XML and temporary identities unchanged.";
    auto* question = new QMessageBox(QMessageBox::Warning, "Repair missing connection IDs", text, QMessageBox::Yes | QMessageBox::Cancel, this);
    question->setObjectName("confirmIdRepair"); question->setTextFormat(Qt::PlainText);
    question->setDefaultButton(QMessageBox::Cancel); question->setAttribute(Qt::WA_DeleteOnClose); fileDialogs_.insert(id, question);
    connect(question, &QDialog::finished, this, [this, id, generation, revision](int answer) {
        fileDialogs_.remove(id); if (answer != QMessageBox::Yes || closeApproved_) return;
        auto apply = [this, id, generation, revision](std::optional<Secret> replacement) {
            const auto doc = context(id);
            if (!doc || !doc->canRepairIds() || doc->status().generation != generation || doc->status().revision != revision || closeApproved_) {
                fail("The owning file changed or was locked. No ID repair was saved."); return;
            }
            try {
                const auto candidate = doc->prepareIdRepair();
                files_->repairIds(id, std::move(replacement), candidate.reservation, [this, id](DocumentReply reply) {
                    fileReply(std::move(reply), {}, [this, id] { reviewFilePasswords(id); });
                });
            } catch (const std::exception&) { fail("Cannot reserve this file's ID repair."); }
        };
        const auto doc = context(id); if (!doc || !doc->canRepairIds()) return;
        apply({});
    }); question->open();
}
void MainWindow::reviewFilePasswords(DocumentId id, std::function<void()> next) {
    const auto doc = context(id); if (!doc || closeApproved_) return;
    if (!doc->status().reviewRequired) { if (next) next(); return; }
    if (!doc->canWrite()) return;
    if (fileDialogs_.value(id)) {
        if (const auto continuations = fileReviews_.value(id); continuations && next) continuations->push_back(std::move(next));
        fileDialogs_.value(id)->raise(); return;
    }
    std::vector<PasswordAuthorization> reviewed; QStringList accounts;
    for (auto it = doc->profiles().cbegin(); it != doc->profiles().cend(); ++it) {
        if (it.value()["kind"] != "connection" || !doc->status().passwords.value(it.key()).present) continue;
        const auto effective = it.value()["effective"].toObject();
        const auto target = passwordAccount(effective); if (!target) continue;
        if (reviewed.size() == 1024) { fail("Review at most 1024 consuming connections in one file."); return; }
        reviewed.push_back({it.key(), *target});
        accounts.append(accountLabel(it.value()["name"].toString(), *target));
    }
    if (accounts.isEmpty()) { if (next) next(); return; }
    const auto generation = doc->status().generation; const auto revision = doc->status().revision;
    auto* question = new QMessageBox(QMessageBox::Warning, "Review imported passwords",
        "This native file has no authenticated password-to-account bindings. Its displayed endpoints, accounts and password sources are not authenticated by the password ciphertext. Removing all private extensions is indistinguishable from a legitimate native file.\n\n"
        "Allow password reuse only by these committed connections? Changing an account, endpoint or password source removes authorization. Original mRemoteNG resaves may remove the private bindings and require another review.\n\n" + accounts.join('\n') +
        "\n\nReview changes only the in-memory candidate. Save connection file explicitly to persist the bindings. Cancel leaves passwords unavailable for reuse and does not change the XML.",
        QMessageBox::Yes | QMessageBox::Cancel, this);
    question->setObjectName("nativePasswordReview"); question->setTextFormat(Qt::PlainText);
    question->setDefaultButton(QMessageBox::Cancel); question->setAttribute(Qt::WA_DeleteOnClose);
    fileDialogs_.insert(id, question);
    auto continuations = std::make_shared<std::vector<std::function<void()>>>();
    if (next) continuations->push_back(std::move(next)); fileReviews_.insert(id, continuations);
    connect(question, &QDialog::finished, this, [this, id, generation, revision, reviewed = std::move(reviewed), continuations](int answer) mutable {
        fileDialogs_.remove(id); fileReviews_.remove(id);
        if (answer != QMessageBox::Yes || closeApproved_) {
            if (credentialAttempt_ && credentialAttempt_->document == id) cancelCredentials();
            return;
        }
        const auto doc = context(id);
        if (!doc || !doc->canWrite() || doc->status().generation != generation || doc->status().revision != revision) {
            fail("The owning file changed or was locked. Unlock and review its current accounts again."); return;
        }
        files_->reviewPasswords(id, std::move(reviewed), [this, continuations](DocumentReply reply) {
            fileReply(std::move(reply), {}, [continuations] { for (const auto& next : *continuations) next(); });
        });
    });
    connect(question, &QObject::destroyed, this, [this] { QTimer::singleShot(0, this, &MainWindow::processFileUnlocks); });
    question->setWindowModality(Qt::NonModal); question->show();
}
void MainWindow::savePasswordEdit(std::function<void()> next) {
    if (!pendingPassword_ || pendingPassword_->profile != ProfileReference{editorDocument_, editor_->id()}) {
        autoSaveFailed("Re-enter the pending password or Revert before continuing."); return;
    }
    const auto reference = pendingPassword_->profile; const auto doc = context(reference.document);
    if (!doc || !doc->canWrite()) { autoSaveFailed("Unlock this file before saving its password."); return; }
    const auto& edit = pendingPassword_->edit;
    std::vector<PasswordAuthorization> reviewed;
    if (edit.kind != PasswordEdit::Kind::Clear) {
        for (auto it = doc->profiles().cbegin(); it != doc->profiles().cend(); ++it) {
            if (it.value()["kind"] != "connection") continue;
            QString source = it.key(); bool affected = false, present = false;
            while (!source.isEmpty()) {
                const auto state = doc->status().passwords.value(source);
                const bool editing = source == reference.profile;
                affected = affected || editing;
                const bool inherited = editing ? edit.kind == PasswordEdit::Kind::Inheritance && edit.inherited : state.inherited;
                const auto parent = doc->profiles().value(source)["parent"].toString();
                if (!inherited || parent.isEmpty()) {
                    present = editing && edit.kind == PasswordEdit::Kind::Set ? true : state.local; break;
                }
                source = parent;
            }
            if (!affected || !present) continue;
            const auto effective = it.value()["effective"].toObject();
            const auto target = passwordAccount(effective); if (!target) continue;
            if (reviewed.size() == 1024) { autoSaveFailed("Review at most 1024 consuming connections in one password edit."); return; }
            reviewed.push_back({it.key(), *target});
        }
    }
    if (closeApproved_) { autoSaveFailed("Password edit canceled. Re-enter the password or Revert."); return; }
    // Set is explicit consent to store a password and to bind it to every
    // committed endpoint/account that consumes it; no separate review prompt.
    commitPasswordEdit(std::move(reviewed), std::move(next));
}
void MainWindow::commitPasswordEdit(std::vector<PasswordAuthorization> reviewed, std::function<void()> next) {
    if (!pendingPassword_) { autoSaveFailed("Re-enter the password or Revert."); return; }
    const auto reference = pendingPassword_->profile; const auto doc = context(reference.document);
    if (!doc || !doc->canWrite() || reference != ProfileReference{editorDocument_, editor_->id()}) {
        autoSaveFailed("The owning file is locked, busy or awaiting recovery."); return;
    }
    try {
        const auto input = QJsonDocument(editor_->draft()).toJson(QJsonDocument::Compact);
        const auto candidate = doc->prepareProfile(rm::ProfileChange::Update, input);
        std::optional<Secret> master;
        if (pendingPassword_->replacement) master = pendingPassword_->replacement->clone();
        files_->editPassword(reference, candidate.projection, pendingPassword_->edit.clone(), std::move(reviewed), std::move(master),
            candidate.reservation, [this, reference, next = std::move(next)](DocumentReply reply) mutable {
                fileReply(std::move(reply), reference.profile, std::move(next), true);
            });
    } catch (const std::exception&) { autoSaveFailed("Cannot prepare this file's password edit."); }
}
