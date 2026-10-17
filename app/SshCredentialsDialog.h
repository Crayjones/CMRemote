#pragma once
#include "Secret.h"
#include <QDialog>
#include <functional>
#include <memory>
#include <optional>

namespace Ui { class SshCredentialsDialog; }
struct SshCredentialRequest {
    QString endpoint, username, auth, keyPath;
    bool usernameEditable = true, methodEditable = false;
};
struct TransientSshCredentials {
    QString username, auth, keyPath;
    Secret authentication;
};
class SshCredentialsDialog final : public QDialog {
    Q_OBJECT
public:
    using Completion = std::function<void(std::optional<TransientSshCredentials>)>;
    SshCredentialsDialog(const SshCredentialRequest& request, Completion complete, QWidget* parent = nullptr);
    ~SshCredentialsDialog() override;
    void accept() override;
private:
    void methodChanged();
    void clearInput();
    std::unique_ptr<Ui::SshCredentialsDialog> ui_;
    bool methodEditable_ = false, completed_ = false;
    quint64 methodGeneration_ = 0;
};
