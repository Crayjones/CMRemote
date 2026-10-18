#include "Credentials.h"
#include "ui_CredentialsDialog.h"
#include <QDialog>
QDialog* PromptCredentialProvider::request(QWidget* parent, const CredentialRequest& target, Completion complete) {
    auto* dialog = new QDialog(parent);
    Ui::CredentialsDialog ui; ui.setupUi(dialog);
    dialog->setWindowModality(Qt::WindowModal); dialog->setAttribute(Qt::WA_DeleteOnClose);
    ui.endpointLabel->setTextFormat(Qt::PlainText); ui.endpointLabel->setText(target.endpoint);
    ui.usernameEdit->setText(target.username); ui.usernameEdit->setEnabled(target.usernameEditable);
    ui.domainEdit->setText(target.domain); ui.domainEdit->setEnabled(target.usernameEditable);
    ui.clipboardCheck->setVisible(target.clipboardOption);
    ui.saveCheck->setVisible(target.saveOption);
    QObject::connect(ui.saveCheck, &QCheckBox::toggled, dialog, [ui](bool save) {
        ui.hintLabel->setText(save ? "The connection, username and password are saved to a connection file." : "Password is used for this session only. It is not saved.");
    });
    ui.passwordEdit->setFocus();
    QObject::connect(dialog, &QDialog::finished, parent ? parent : dialog, [ui, saveOption = target.saveOption, complete = std::move(complete)](int answer) {
        std::optional<TransientCredentials> credentials;
        if (answer == QDialog::Accepted) {
            credentials.emplace(); credentials->username = ui.usernameEdit->text(); credentials->domain = ui.domainEdit->text();
            credentials->password = ui.passwordEdit->text().toUtf8(); credentials->clipboard = ui.clipboardCheck->isChecked();
            credentials->save = saveOption && ui.saveCheck->isChecked();
        }
        ui.passwordEdit->clear(); complete(std::move(credentials));
    });
    dialog->open(); return dialog;
}
