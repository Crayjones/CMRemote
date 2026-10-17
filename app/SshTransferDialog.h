#pragma once
#include "RdpTarget.h"
#include "Secret.h"
#include "SshHostKeyStore.h"
#include <QDialog>
#include <QFile>
#include <QPointer>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTimer;
class SshHostKeyService;
class SshWorker;

// mRemoteNG's "SSH File Transfer": uploads one local file with SCP. The connection
// uses the same host-key trust, authentication and stream bounds as SSH sessions.
class SshTransferDialog final : public QDialog {
    Q_OBJECT
public:
    SshTransferDialog(SessionTarget target, SshHostKeyService* trust, QWidget* parent = nullptr);
    ~SshTransferDialog() override;
    void setPassword(Secret password);
    void setLocalFile(const QString& path);
    void setRemotePath(const QString& path);
    void start();
    bool running() const { return worker_ != nullptr; }
    // The command run on the server for a remote destination.
    static QString scpCommand(const QString& remote);
signals:
    void finished(bool ok, QString message);
private:
    enum class Step { Idle, Ready, Header, Data, Final };
    void received(const QByteArray& bytes);
    void sendData();
    void finish(bool ok, const QString& message);
    void inspectHost(const QString& request, SshHostKey key, quint64 generation);
    SessionTarget target_;
    QPointer<SshHostKeyService> trust_;
    QLineEdit *host_, *port_, *user_, *password_, *local_, *remote_;
    QLabel* status_;
    QProgressBar* progress_;
    QPushButton* transfer_;
    QTimer* retry_;
    QPointer<SshWorker> worker_;
    QFile file_;
    QByteArray acks_;
    Step step_ = Step::Idle;
    qint64 sent_ = 0;
    quint64 generation_ = 0;
    QString trustRequest_;
    bool passwordPrefilled_ = false;
};
