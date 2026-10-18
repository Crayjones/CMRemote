#pragma once
#include <QByteArray>
#include <QString>
#include <functional>
#include <optional>
class QWidget;
class QDialog;
struct TransientCredentials {
    QString username, domain;
    QByteArray password;
    bool clipboard = false;
    bool save = false; // Also save the connection and password to a connection file.
    TransientCredentials() = default;
    TransientCredentials(const TransientCredentials&) = delete;
    TransientCredentials& operator=(const TransientCredentials&) = delete;
    TransientCredentials(TransientCredentials&&) = default;
    TransientCredentials& operator=(TransientCredentials&&) = default;
    ~TransientCredentials() { password.fill(0); }
};
struct CredentialRequest { QString endpoint, username; bool usernameEditable = true; QString domain; bool clipboardOption = true; bool saveOption = false; };
class CredentialProvider {
public:
    using Completion = std::function<void(std::optional<TransientCredentials>)>;
    virtual ~CredentialProvider() = default;
    virtual QDialog* request(QWidget* parent, const CredentialRequest& target, Completion complete) = 0;
};
class PromptCredentialProvider final : public CredentialProvider {
public:
    QDialog* request(QWidget* parent, const CredentialRequest& target, Completion complete) override;
};
