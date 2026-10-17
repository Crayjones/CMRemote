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
    ui.passwordEdit->setFocus();
    QObject::connect(dialog, &QDialog::finished, parent ? parent : dialog, [ui, complete = std::move(complete)](int answer) {
        std::optional<TransientCredentials> credentials;
        if (answer == QDialog::Accepted) {
            credentials.emplace(); credentials->username = ui.usernameEdit->text(); credentials->domain = ui.domainEdit->text();
            credentials->password = ui.passwordEdit->text().toUtf8(); credentials->clipboard = ui.clipboardCheck->isChecked();
        }
        ui.passwordEdit->clear(); complete(std::move(credentials));
    });
    dialog->open(); return dialog;
}
