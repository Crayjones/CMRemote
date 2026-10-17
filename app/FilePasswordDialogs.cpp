#include "FilePasswordDialogs.h"
#include "ConnectionCrypto.h"
#include "ui_FilePasswordDialog.h"
#include <QDialog>

QDialog* FilePasswordDialogs::passphrase(QWidget* parent, Mode mode, PassphraseCompletion complete) {
    auto* dialog = new QDialog(parent); Ui::FilePasswordDialog ui; ui.setupUi(dialog);
    dialog->setObjectName("filePassword"); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowModality(Qt::WindowModal);
    const bool confirm = mode != Mode::Unlock; ui.masterConfirmEdit->setVisible(confirm); ui.confirmLabel->setVisible(confirm);
    // Retain one overflow character so validation rejects oversized input.
    ui.masterEdit->setMaxLength(4097); ui.masterConfirmEdit->setMaxLength(4097);
    dialog->setWindowTitle(confirm ? "Set or change file password" : "Unlock connection file");
    ui.hintLabel->setText(confirm ? "Enter the same exact file password twice. Lost passwords cannot be recovered. Old copies still use their old passwords." :
        "Enter this file's exact master password. Cancel leaves this file locked and unchanged.");
    QObject::connect(ui.buttonBox, &QDialogButtonBox::accepted, dialog, [dialog, ui, confirm] {
        const auto text = ui.masterEdit->text();
        auto bytes = text.toUtf8(); const Secret candidate(bytes); bytes.fill(0);
        const bool valid = confirm ? ConnectionCrypto::validNewMaster(candidate) : ConnectionCrypto::encodeMaster(candidate).has_value();
        if (!valid) { ui.errorLabel->setText(confirm ? "Use 1–4096 single-byte characters (U+0001–U+00FF). A custom password cannot be whitespace-only or mR3m." : "Enter 1–4096 single-byte characters (U+0000–U+00FF), exactly as used by this file."); return; }
        if (confirm && text != ui.masterConfirmEdit->text()) { ui.errorLabel->setText("Enter the same exact password in both fields."); return; }
        dialog->accept();
    });
    QObject::connect(ui.buttonBox, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(dialog, &QDialog::finished, parent ? parent : dialog, [ui, complete = std::move(complete)](int answer) mutable {
        std::optional<Secret> secret;
        if (answer == QDialog::Accepted) { auto bytes = ui.masterEdit->text().toUtf8(); secret.emplace(bytes); bytes.fill(0); }
        ui.masterEdit->clear(); ui.masterConfirmEdit->clear(); complete(std::move(secret));
    });
    dialog->open(); ui.masterEdit->setFocus(); return dialog;
}
