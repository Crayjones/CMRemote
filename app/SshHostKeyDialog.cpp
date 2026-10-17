#include "SshHostKeyDialog.h"
#include <QPushButton>
#include <stdexcept>

namespace {
QString endpoint(const SshHostKey& key) { return (key.host.contains(':') ? '[' + key.host + ']' : key.host) + ':' + QString::number(key.port); }
}
SshHostKeyDialog::SshHostKeyDialog(const SshHostKeyInspection& inspection, Completion complete, QWidget* parent) : QMessageBox(parent) {
    const bool changed = inspection.status == SshHostKeyStatus::Changed;
    if (inspection.status == SshHostKeyStatus::Trusted || (changed && !inspection.previous)) throw std::invalid_argument("SSH host-key prompt requires an unknown or changed key.");
    setWindowTitle(changed ? "SSH server key changed" : "Unknown SSH server key"); setTextFormat(Qt::PlainText);
    setIcon(changed ? QMessageBox::Warning : QMessageBox::Question);
    QString message = changed ? "The SSH server key has changed. The server may have been replaced or the connection may be intercepted.\n\n" : "This SSH server key is not trusted yet.\n\n";
    message += "Host: " + endpoint(inspection.candidate) + "\nAlgorithm: " + inspection.candidate.algorithm;
    if (changed) message += "\nPreviously trusted key: " + inspection.previous->fingerprint;
    message += "\nNew server key: " + inspection.candidate.fingerprint + "\n\nVerify the server key before continuing."; setText(message);
    auto* save = addButton(changed ? "Replace trusted key and connect" : "Trust and save", QMessageBox::AcceptRole); save->setObjectName("sshTrustSave");
    auto* once = addButton(changed ? "Connect once without replacement" : "Connect once", QMessageBox::ActionRole); once->setObjectName("sshTrustOnce");
    configure(std::move(complete), save, changed ? SshHostKeyDecision::ReplaceAndConnect : SshHostKeyDecision::TrustAndSave, once);
}
SshHostKeyDialog::SshHostKeyDialog(const SshHostKeyReply& reply, Completion complete, QWidget* parent) : QMessageBox(parent) {
    if (!reply.committed || reply.allowed || reply.result.warning.isEmpty()) throw std::invalid_argument("SSH save-warning prompt requires a committed save warning.");
    setWindowTitle("SSH host key saved with a warning"); setTextFormat(Qt::PlainText); setIcon(QMessageBox::Warning);
    setText(reply.result.warning + "\n\nThe trusted key was saved. Cancel stops this connection without undoing the saved key.");
    auto* acknowledge = addButton("Acknowledge and connect", QMessageBox::AcceptRole); acknowledge->setObjectName("sshTrustAcknowledge");
    configure(std::move(complete), acknowledge, SshHostKeyDecision::AcknowledgeWarning);
}
void SshHostKeyDialog::configure(Completion complete, QPushButton* save, SshHostKeyDecision savedChoice, QPushButton* once) {
    setWindowModality(Qt::WindowModal); setAttribute(Qt::WA_DeleteOnClose);
    auto* cancel = addButton("Cancel", QMessageBox::RejectRole); cancel->setObjectName("sshTrustCancel"); setDefaultButton(cancel); setEscapeButton(cancel);
    connect(this, &QDialog::finished, this, [this, complete = std::move(complete), save, savedChoice, once](int) mutable {
        if (completed_) return;
        completed_ = true; const auto* chosen = clickedButton();
        complete(chosen == save ? savedChoice : once && chosen == once ? SshHostKeyDecision::ConnectOnce : SshHostKeyDecision::Cancel);
    });
}
