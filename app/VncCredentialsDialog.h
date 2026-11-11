#pragma once
#include "Secret.h"
#include <QDialog>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

namespace Ui { class VncCredentialsDialog; }
struct VncCredentials {
    QString username;
    // nullopt: use the saved password.
    std::optional<Secret> password;
};
struct VncCredentialsRequest {
    bool username = false;
    QString initialUsername;
    // A saved password exists: an empty password field uses it.
    bool savedPassword = false;
};
// mRemoteNG-style "Password" prompt, shown only when a VNC server asks for
// credentials that are not saved. The VeNCrypt Plain types also ask for a
// username. Everything entered is session-only.
class VncCredentialsDialog final : public QDialog {
    Q_OBJECT
public:
    using Request = VncCredentialsRequest;
    using Completion = std::function<void(std::optional<VncCredentials>)>;
    VncCredentialsDialog(Completion complete, QWidget* parent = nullptr, Request request = {});
    ~VncCredentialsDialog() override;
    void accept() override;
private:
    void clearInput();
    std::unique_ptr<Ui::VncCredentialsDialog> ui_;
    const Request request_;
    bool completed_ = false;
};
