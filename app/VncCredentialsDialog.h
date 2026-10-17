#pragma once
#include "Secret.h"
#include <QDialog>
#include <functional>
#include <memory>
#include <optional>

namespace Ui { class VncCredentialsDialog; }
// mRemoteNG-style "Password" prompt, shown only when a VNC server requires a
// password and none is saved. The password is session-only.
class VncCredentialsDialog final : public QDialog {
    Q_OBJECT
public:
    using Completion = std::function<void(std::optional<Secret>)>;
    VncCredentialsDialog(Completion complete, QWidget* parent = nullptr);
    ~VncCredentialsDialog() override;
    void accept() override;
private:
    void clearInput();
    std::unique_ptr<Ui::VncCredentialsDialog> ui_;
    bool completed_ = false;
};
