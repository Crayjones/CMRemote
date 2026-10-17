#include "SshCredentialsDialog.h"
#include "ui_SshCredentialsDialog.h"
#include <QFileDialog>
#include <algorithm>

namespace {
bool controls(const QString& value) { return std::any_of(value.begin(), value.end(), [](QChar c) { return c.category() == QChar::Other_Control; }); }
}
SshCredentialsDialog::SshCredentialsDialog(const SshCredentialRequest& request, Completion complete, QWidget* parent)
    : QDialog(parent), ui_(std::make_unique<Ui::SshCredentialsDialog>()), methodEditable_(request.methodEditable) {
    ui_->setupUi(this); setWindowModality(Qt::WindowModal); setAttribute(Qt::WA_DeleteOnClose);
    ui_->endpointLabel->setText(request.endpoint); ui_->usernameEdit->setText(request.username); ui_->usernameEdit->setEnabled(request.usernameEditable);
    ui_->authCombo->addItem("Password", "password"); ui_->authCombo->addItem("Private key", "private_key");
    ui_->authCombo->setCurrentIndex(ui_->authCombo->findData(request.auth)); ui_->authCombo->setEnabled(methodEditable_);
    ui_->keyPathEdit->setText(request.keyPath); ui_->keyPathEdit->setReadOnly(!methodEditable_); methodChanged();
    connect(ui_->authCombo, &QComboBox::currentIndexChanged, this, [this] { methodChanged(); });
    connect(ui_->chooseKeyButton, &QPushButton::clicked, this, [this] {
        if (completed_ || !methodEditable_ || ui_->authCombo->currentData().toString() != "private_key") return;
        const auto generation = methodGeneration_;
        auto* picker = new QFileDialog(this, "Choose SSH key file"); picker->setObjectName("sshCredentialKeyPicker");
        picker->setFileMode(QFileDialog::ExistingFile); picker->setNameFilter("All files (*)");
        picker->setWindowModality(Qt::WindowModal); picker->setAttribute(Qt::WA_DeleteOnClose);
        connect(picker, &QFileDialog::fileSelected, this, [this, generation](const QString& path) {
            if (!completed_ && generation == methodGeneration_) ui_->keyPathEdit->setText(path);
        });
        picker->open();
    });
    connect(this, &QDialog::finished, this, [this, complete = std::move(complete)](int answer) mutable {
        if (completed_) return;
        completed_ = true; std::optional<TransientSshCredentials> reply;
        if (answer == QDialog::Accepted) {
            reply.emplace(); reply->username = ui_->usernameEdit->text(); reply->auth = ui_->authCombo->currentData().toString(); reply->keyPath = ui_->keyPathEdit->text();
            auto text = ui_->authenticationEdit->text(); auto bytes = text.toUtf8(); reply->authentication = Secret(bytes);
            bytes.fill(0); text.fill(QChar(0));
        }
        clearInput();
        for (auto* picker : findChildren<QFileDialog*>("sshCredentialKeyPicker")) picker->reject();
        complete(std::move(reply));
    });
    ui_->authenticationEdit->setFocus();
}
SshCredentialsDialog::~SshCredentialsDialog() { clearInput(); }
void SshCredentialsDialog::clearInput() {
    auto text = ui_->authenticationEdit->text(); ui_->authenticationEdit->setText({}); text.fill(QChar(0));
}
void SshCredentialsDialog::methodChanged() {
    ++methodGeneration_; clearInput(); ui_->errorLabel->clear();
    const bool key = ui_->authCombo->currentData().toString() == "private_key";
    ui_->authenticationLabel->setText(key ? "Passphrase" : "Password");
    ui_->keyPathLabel->setVisible(key); ui_->keyPathEdit->setVisible(key); ui_->chooseKeyButton->setVisible(key && methodEditable_);
}
void SshCredentialsDialog::accept() {
    if (completed_) return;
    const auto method = ui_->authCombo->currentData().toString();
    if (method != "password" && method != "private_key") { ui_->errorLabel->setText("Choose Password or Private key."); return; }
    const auto username = ui_->usernameEdit->text(), path = ui_->keyPathEdit->text();
    if (username.toUtf8().size() > 4096 || controls(username)) { ui_->errorLabel->setText("Invalid SSH username."); return; }
    if (path.toUtf8().size() > 4096 || controls(path) || (method == "private_key" && path.isEmpty())) {
        ui_->errorLabel->setText("Choose a valid OpenSSH private-key path."); return;
    }
    auto text = ui_->authenticationEdit->text(); auto bytes = text.toUtf8(); const bool bounded = bytes.size() <= 4096;
    bytes.fill(0); text.fill(QChar(0));
    if (!bounded) { ui_->errorLabel->setText("SSH authentication must be at most 4096 UTF-8 bytes."); return; }
    QDialog::accept();
}
