#pragma once
#include "SshHostKeyStore.h"
#include <QMessageBox>
#include <functional>

class SshHostKeyDialog final : public QMessageBox {
    Q_OBJECT
public:
    using Completion = std::function<void(SshHostKeyDecision)>;
    SshHostKeyDialog(const SshHostKeyInspection& inspection, Completion complete, QWidget* parent = nullptr);
    SshHostKeyDialog(const SshHostKeyReply& committedWarning, Completion complete, QWidget* parent = nullptr);
private:
    void configure(Completion complete, QPushButton* save, SshHostKeyDecision savedChoice, QPushButton* once = nullptr);
    bool completed_ = false;
};
