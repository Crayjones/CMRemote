#include "MainWindow.h"
#include "SessionPane.h"
#include "SshCredentialsDialog.h"
#include "VncAuthentication.h"
#include <exception>
#include <QDialog>
#include <QJsonArray>
#include <QTimer>

std::optional<SessionTarget> MainWindow::committedTarget(const QString& id) const {
    const auto profile = profiles_.value(id), effective = profile.value("effective").toObject();
    if (profile.value("kind") != "connection") return std::nullopt;
    return normalizeSessionTarget(sessionTarget(effective));
}
bool MainWindow::validAttempt(const Attempt& attempt) const {
    if (!attempt || credentialAttempt_ != attempt || attempt->token != credentialToken_ || closeApproved_) return false;
    if (!attempt->owner || !sessionPanels().contains(attempt->owner) || (attempt->replacing && (!attempt->page || !ownerOf(attempt->page)))) return false;
    if (!attempt->profile.isEmpty()) {
        const auto doc = context(attempt->document);
        if (!doc || doc->status().generation != attempt->generation || doc->status().revision != attempt->revision) return false;
        const auto effective = doc->profiles().value(attempt->profile).value("effective").toObject();
        const auto target = expectedTarget(*attempt);
        if (!target || !(*target == attempt->target) || !effective.value("unsupported").toArray().isEmpty()) return false;
    }
    return true;
}
void MainWindow::clearCredentialDialog() { auto dialog = credentialDialog_; credentialDialog_.clear(); if (dialog) dialog->accept(); }
void MainWindow::cancelCredentials() {
    ++credentialToken_; auto dialog = credentialDialog_; credentialDialog_.clear(); credentialPage_.clear(); credentialOwner_.clear();
    if (credentialAttempt_) { credentialAttempt_->savePassword.reset(); credentialAttempt_->credentials.reset(); credentialAttempt_->sshAuthentication.reset(); credentialAttempt_->vncAuthentication.reset(); } credentialAttempt_.reset();
    if (dialog) dialog->reject();
    if (!closeApproved_) updateFileActions();
    if (folderConnect_ && !closeApproved_) QTimer::singleShot(0, this, [this] { continueFolderConnect(); });
    if (!reconnectQueue_.isEmpty()) QTimer::singleShot(0, this, [this] { continueReconnectAll(); });
}
void MainWindow::promptCredentials(Attempt attempt) {
    if (!validAttempt(attempt)) return;
    if (attempt->target.protocol == "vnc") {
        // No saved password: connect and ask only if the server requires one
        // (SessionPane shows the Password prompt during the handshake).
        attempt->vncAuthentication = vncPromptAuthentication(); finishCredentials(attempt); return;
    }
    if (attempt->target.protocol == "ssh") {
        const bool freshTemporary = attempt->profile.isEmpty() && !attempt->replacing;
        auto* dialog = new SshCredentialsDialog({attempt->target.endpoint(), attempt->target.username, attempt->target.sshAuth,
            attempt->target.sshKeyPath, !attempt->replacing || !attempt->profile.isEmpty(), freshTemporary, freshTemporary},
            [this, attempt](std::optional<TransientSshCredentials> credentials) {
                if (!validAttempt(attempt)) return; credentialDialog_.clear();
                if (!credentials) { cancelCredentials(); return; }
                auto target = attempt->target; target.username = credentials->username;
                target.sshAuth = credentials->auth; target.sshKeyPath = credentials->keyPath;
                attempt->save = credentials->save;
                if (attempt->save && target.sshAuth == "password" && credentials->authentication.size()) attempt->savePassword = credentials->authentication.clone();
                const auto normalized = normalizeSshTarget(target);
                if (!normalized || normalized->username.isEmpty()) { cancelCredentials(); fail("Invalid SSH username or target."); return; }
                try {
                    attempt->sshAuthentication = target.sshAuth == "private_key" ?
                        sshPrivateKeyAuthentication(target.sshKeyPath, std::move(credentials->authentication)) :
                        sshPasswordAuthentication(std::move(credentials->authentication));
                } catch (const rust::Error&) { cancelCredentials(); fail("SSH authentication exceeds supported limits."); return; }
                attempt->authenticatedTarget = *normalized; finishCredentials(attempt);
            }, this);
        credentialDialog_ = dialog; dialog->open(); return;
    }
    PromptCredentialProvider provider;
    const bool freshTemporary = attempt->profile.isEmpty() && !attempt->replacing;
    credentialDialog_ = provider.request(this, {attempt->target.endpoint(), attempt->target.username, !attempt->replacing || !attempt->profile.isEmpty(), attempt->target.domain, true, freshTemporary},
        [this, attempt](std::optional<TransientCredentials> credentials) {
            if (!validAttempt(attempt)) return; credentialDialog_.clear();
            if (!credentials) { cancelCredentials(); return; }
            auto target = attempt->target; target.username = credentials->username; target.domain = credentials->domain;
            if (!normalizeSessionTarget(target)) { cancelCredentials(); fail("Invalid RDP username or target."); return; }
            attempt->save = credentials->save;
            if (attempt->save && !credentials->password.isEmpty()) attempt->savePassword = Secret(credentials->password);
            attempt->credentials = std::move(credentials); finishCredentials(attempt);
        });
}
void MainWindow::saveAttempt(const Attempt& attempt, const SessionTarget& target) {
    if (!attempt->save || closeApproved_) return;
    attempt->save = false;
    std::shared_ptr<Secret> password;
    if (attempt->savePassword) password = std::make_shared<Secret>(std::move(*attempt->savePassword));
    attempt->savePassword.reset();
    guard([this, target, password] { saveTargetToFile(target, password); });
}
void MainWindow::finishCredentials(Attempt attempt) {
    if (!validAttempt(attempt)) return;
    if (attempt->target.protocol == "vnc") {
        if (!attempt->vncAuthentication) return;
        clearCredentialDialog(); credentialAttempt_.reset(); credentialPage_.clear(); credentialOwner_.clear();
        auto authentication = std::move(attempt->vncAuthentication); attempt->vncAuthentication.reset();
        attachSession(attempt->profile, attempt->target, false, attempt->owner, attempt->page, {}, attempt->document, {}, std::move(authentication));
        updateFileActions(); return;
    }
    if (attempt->target.protocol == "ssh") {
        if (!attempt->sshAuthentication) {
            if (!attempt->credentials || attempt->target.sshAuth != "password") return;
            try { attempt->sshAuthentication = sshPasswordAuthentication(Secret(std::move(attempt->credentials->password))); }
            catch (const rust::Error&) { cancelCredentials(); fail("SSH authentication exceeds supported limits."); return; }
            attempt->credentials.reset();
        }
        const auto target = attempt->authenticatedTarget.value_or(attempt->target);
        if (target.username.isEmpty()) { cancelCredentials(); fail("SSH requires a username."); return; }
        clearCredentialDialog(); credentialAttempt_.reset(); credentialPage_.clear(); credentialOwner_.clear();
        auto authentication = std::move(attempt->sshAuthentication); attempt->sshAuthentication.reset();
        attachSession(attempt->profile, target, false, attempt->owner, attempt->page, {}, attempt->document, std::move(authentication));
        saveAttempt(attempt, target);
        updateFileActions(); if (folderConnect_) QTimer::singleShot(0, this, [this] { continueFolderConnect(); });
        if (!reconnectQueue_.isEmpty()) QTimer::singleShot(0, this, [this] { continueReconnectAll(); }); return;
    }
    if (!attempt->credentials) return;
    auto target = attempt->target;
    if (!attempt->replacing || !attempt->profile.isEmpty()) { target.username = attempt->credentials->username; target.domain = attempt->credentials->domain; }
    const auto normalized = normalizeSessionTarget(target); if (!normalized) { cancelCredentials(); fail("Invalid RDP username or target."); return; }
    clearCredentialDialog(); credentialAttempt_.reset(); credentialPage_.clear(); credentialOwner_.clear();
    auto credentials = std::move(attempt->credentials); attempt->credentials.reset();
    attachSession(attempt->profile, *normalized, false, attempt->owner, attempt->page, std::move(credentials), attempt->document);
    saveAttempt(attempt, *normalized); updateFileActions();
    if (folderConnect_) QTimer::singleShot(0, this, [this] { continueFolderConnect(); });
}
void MainWindow::connectDifferent() {
    const ProfileReference reference{selectedDocument(), selectedId()};
    guard([this, reference] {
        if (!selectConnectionProfile(reference)) return;
        const auto id = selectedId(); const auto target = committedTarget(id); if (!target) return;
        for (auto* pane : sessionPages()) if (pane->profile() == id && pane->document() == editorDocument_) { reconnectPane(pane, true); return; }
        requestSession(id, *target, false, currentSessions(), nullptr, true, editorDocument_);
    });
}
