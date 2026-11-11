#include "VncCredentialsDialog.h"
#include "VncAuthentication.h"
#include "ui_VncCredentialsDialog.h"
#include <algorithm>

namespace {
bool control(QChar c) { return c.unicode() < 0x20 || c.unicode() == 0x7f; }
}
VncCredentialsDialog::VncCredentialsDialog(Completion complete, QWidget* parent, Request request)
    : QDialog(parent), ui_(std::make_unique<Ui::VncCredentialsDialog>()), request_(std::move(request)) {
    ui_->setupUi(this); setObjectName("vncCredentialsDialog");
    // Application-modal, not window-modal: on macOS a window-modal dialog becomes
    // a title-less sheet, hiding the title that gives the prompt context.
    setWindowModality(Qt::ApplicationModal); setAttribute(Qt::WA_DeleteOnClose);
    // Title and close button only, like mRemoteNG's Password prompt.
    setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    if (request_.username) {
        setWindowTitle("Username and password");
        ui_->usernameEdit->setText(request_.initialUsername);
    } else ui_->usernameEdit->hide();
    if (request_.savedPassword) ui_->passwordEdit->setPlaceholderText("Saved password");
    ui_->errorLabel->hide();
    connect(this, &QDialog::finished, this, [this, complete = std::move(complete)](int answer) mutable {
        if (completed_) return;
        completed_ = true; std::optional<VncCredentials> reply;
        if (answer == QDialog::Accepted) {
            reply.emplace(); reply->username = request_.username ? ui_->usernameEdit->text() : QString();
            auto text = ui_->passwordEdit->text();
            if (!text.isEmpty()) { auto bytes = text.toUtf8(); reply->password.emplace(bytes); bytes.fill(0); }
            text.fill(QChar(0));
        }
        clearInput(); complete(std::move(reply));
    });
    (request_.username && ui_->usernameEdit->text().isEmpty() ? ui_->usernameEdit : ui_->passwordEdit)->setFocus();
}
VncCredentialsDialog::~VncCredentialsDialog() { clearInput(); }
void VncCredentialsDialog::clearInput() {
    auto text = ui_->passwordEdit->text(); ui_->passwordEdit->setText({}); text.fill(QChar(0));
}
void VncCredentialsDialog::accept() {
    if (completed_) return;
    auto text = ui_->passwordEdit->text();
    const auto size = text.toUtf8().size();
    const bool passwordValid = (size >= 1 || request_.savedPassword) && size_t(size) <= VncAuthentication::MaxPassword &&
        std::none_of(text.begin(), text.end(), control);
    text.fill(QChar(0));
    const auto username = ui_->usernameEdit->text();
    const bool usernameValid = !request_.username ||
        (!username.isEmpty() && username.toUtf8().size() <= 4096 && std::none_of(username.begin(), username.end(), control));
    if (!usernameValid || !passwordValid) {
        ui_->errorLabel->setText(!usernameValid ? "Enter a username without control characters." :
            "Enter a password of up to 4096 bytes without control characters. Nothing is truncated.");
        ui_->errorLabel->show(); return;
    }
    QDialog::accept();
}
