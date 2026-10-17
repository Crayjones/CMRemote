#include "VncCredentialsDialog.h"
#include "ui_VncCredentialsDialog.h"
#include <algorithm>

VncCredentialsDialog::VncCredentialsDialog(Completion complete, QWidget* parent)
    : QDialog(parent), ui_(std::make_unique<Ui::VncCredentialsDialog>()) {
    ui_->setupUi(this); setObjectName("vncCredentialsDialog");
    // Application-modal, not window-modal: on macOS a window-modal dialog becomes
    // a title-less sheet, hiding the "Password" title that gives the prompt context.
    setWindowModality(Qt::ApplicationModal); setAttribute(Qt::WA_DeleteOnClose);
    // Title and close button only, like mRemoteNG's Password prompt.
    setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    ui_->errorLabel->hide();
    connect(this, &QDialog::finished, this, [this, complete = std::move(complete)](int answer) mutable {
        if (completed_) return;
        completed_ = true; std::optional<Secret> reply;
        if (answer == QDialog::Accepted) {
            auto text = ui_->passwordEdit->text(); auto bytes = text.toLatin1(); reply.emplace(bytes);
            bytes.fill(0); text.fill(QChar(0));
        }
        clearInput(); complete(std::move(reply));
    });
    ui_->passwordEdit->setFocus();
}
VncCredentialsDialog::~VncCredentialsDialog() { clearInput(); }
void VncCredentialsDialog::clearInput() {
    auto text = ui_->passwordEdit->text(); ui_->passwordEdit->setText({}); text.fill(QChar(0));
}
void VncCredentialsDialog::accept() {
    if (completed_) return;
    auto text = ui_->passwordEdit->text();
    const bool valid = text.size() >= 1 && text.size() <= 8 &&
        std::all_of(text.begin(), text.end(), [](QChar c) { return c.unicode() >= 0x20 && c.unicode() <= 0x7e; });
    text.fill(QChar(0));
    if (!valid) {
        ui_->errorLabel->setText("VNC passwords are 1–8 printable ASCII characters. Nothing is truncated.");
        ui_->errorLabel->show(); return;
    }
    QDialog::accept();
}
