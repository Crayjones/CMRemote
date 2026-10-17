#include "SshTransferDialog.h"
#include "SshHostKeyDialog.h"
#include "SshHostKeyService.h"
#include "SshWorker.h"
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <atomic>

namespace {
std::atomic<quint64> nextGeneration{1};
constexpr qint64 Chunk = 32 * 1024;
QString quoted(const QString& value) { return "'" + QString(value).replace("'", "'\\''") + "'"; }
}

QString SshTransferDialog::scpCommand(const QString& remote) {
    // Keep a leading ~/ outside the quotes so the server shell expands it.
    const auto path = remote.trimmed().isEmpty() ? QString(".") : remote.trimmed();
    if (path == "~") return "scp -t -- ~";
    if (path.startsWith("~/")) return "scp -t -- ~/" + quoted(path.mid(2));
    return "scp -t -- " + quoted(path);
}

SshTransferDialog::SshTransferDialog(SessionTarget target, SshHostKeyService* trust, QWidget* parent)
    : QDialog(parent), target_(std::move(target)), trust_(trust) {
    setObjectName("sshTransferDialog"); setWindowTitle("SSH File Transfer"); resize(520, 0);
    auto* layout = new QVBoxLayout(this); auto* form = new QFormLayout;
    host_ = new QLineEdit(target_.host, this); host_->setObjectName("transferHost"); form->addRow("Host:", host_);
    port_ = new QLineEdit(QString::number(target_.port), this); port_->setObjectName("transferPort"); port_->setValidator(new QIntValidator(1, 65535, port_));
    form->addRow("Port:", port_);
    user_ = new QLineEdit(target_.username, this); user_->setObjectName("transferUser"); form->addRow("User:", user_);
    password_ = new QLineEdit(this); password_->setObjectName("transferPassword"); password_->setEchoMode(QLineEdit::Password);
    form->addRow(target_.sshAuth == "private_key" ? "Key passphrase:" : "Password:", password_);
    auto* localRow = new QHBoxLayout; local_ = new QLineEdit(this); local_->setObjectName("transferLocal");
    auto* browse = new QPushButton("Browse…", this); localRow->addWidget(local_); localRow->addWidget(browse);
    form->addRow("Local file:", localRow);
    remote_ = new QLineEdit("~/", this); remote_->setObjectName("transferRemote"); form->addRow("Remote file or folder:", remote_);
    layout->addLayout(form);
    progress_ = new QProgressBar(this); progress_->setObjectName("transferProgress"); progress_->setRange(0, 100); progress_->setValue(0);
    layout->addWidget(progress_);
    status_ = new QLabel(this); status_->setObjectName("transferStatus"); status_->setWordWrap(true); layout->addWidget(status_);
    auto* buttons = new QDialogButtonBox(this);
    transfer_ = buttons->addButton("Transfer", QDialogButtonBox::AcceptRole); transfer_->setObjectName("transferStart");
    buttons->addButton(QDialogButtonBox::Close); layout->addWidget(buttons);
    retry_ = new QTimer(this); retry_->setSingleShot(true); retry_->setInterval(10);
    connect(retry_, &QTimer::timeout, this, &SshTransferDialog::sendData);
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, "Choose file to upload"); if (!path.isEmpty()) local_->setText(path);
    });
    connect(transfer_, &QPushButton::clicked, this, &SshTransferDialog::start);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(password_, &QLineEdit::textEdited, this, [this] { passwordPrefilled_ = false; });
}

SshTransferDialog::~SshTransferDialog() {
    if (worker_) worker_->stop();
    if (trust_ && !trustRequest_.isEmpty()) trust_->cancel(trustRequest_);
}

void SshTransferDialog::setPassword(Secret password) {
    // The saved password stays masked; typing replaces it.
    password_->setText(QString::fromUtf8(password.takeBytes())); passwordPrefilled_ = true;
}
void SshTransferDialog::setLocalFile(const QString& path) { local_->setText(path); }
void SshTransferDialog::setRemotePath(const QString& path) { remote_->setText(path); }

void SshTransferDialog::start() {
    if (worker_) return;
    file_.close(); file_.setFileName(local_->text().trimmed());
    const auto name = QFileInfo(file_.fileName()).fileName();
    if (name.isEmpty() || name.contains('\n') || !file_.open(QIODevice::ReadOnly)) { finish(false, "Choose a readable local file."); return; }
    auto target = target_; target.host = host_->text().trimmed(); target.port = quint16(port_->text().toUInt()); target.username = user_->text().trimmed();
    const auto normalized = normalizeSshTarget(target);
    if (!normalized) { finish(false, "Enter a valid host, port and user."); return; }
    std::optional<rust::Box<rm::ssh::Authentication>> authentication;
    try {
        const auto secret = password_->text().toUtf8();
        rust::Vec<uint8_t> bytes; for (const char c : secret) bytes.push_back(uint8_t(c));
        if (normalized->sshAuth == "private_key") {
            const auto path = normalized->sshKeyPath.toUtf8();
            authentication.emplace(rm::ssh::private_key_auth(rust::String(path.constData(), size_t(path.size())), std::move(bytes)));
        } else authentication.emplace(rm::ssh::password_auth(std::move(bytes)));
    } catch (const std::exception& error) { finish(false, QString::fromUtf8(error.what())); return; }
    generation_ = nextGeneration++; acks_.clear(); sent_ = 0; step_ = Step::Ready;
    progress_->setValue(0); status_->setText("Connecting to " + normalized->endpoint() + "…"); transfer_->setEnabled(false);
    try { worker_ = new SshWorker(*normalized, std::move(*authentication), generation_, this, scpCommand(remote_->text())); }
    catch (const std::exception& error) { finish(false, QString::fromUtf8(error.what())); return; }
    connect(worker_, &SshWorker::output, this, [this](QByteArray bytes, quint64 generation) { if (generation == generation_) received(bytes); });
    connect(worker_, &SshWorker::hostKey, this, &SshTransferDialog::inspectHost);
    connect(worker_, &SshWorker::phaseChanged, this, [this](SshWorker::Phase phase, QString error, quint64 generation) {
        if (generation != generation_) return;
        if (phase == SshWorker::Phase::Authenticating) status_->setText("Authenticating…");
        else if (phase == SshWorker::Phase::Connected) status_->setText("Starting SCP…");
        else if ((phase == SshWorker::Phase::Failed || phase == SshWorker::Phase::Disconnected || phase == SshWorker::Phase::Cancelled) && step_ != Step::Idle)
            finish(false, error.isEmpty() ? "The connection closed before the upload finished." : error);
    });
}

void SshTransferDialog::inspectHost(const QString& request, SshHostKey key, quint64 generation) {
    if (generation != generation_ || !worker_) return;
    if (!trust_) { worker_->answerHost(request, generation, false); return; }
    const QPointer<SshTransferDialog> owner(this);
    trustRequest_ = trust_->check(key, generation, [owner, request, key, generation](SshHostKeyReply answer) {
        if (!owner || !owner->worker_ || owner->generation_ != generation) return;
        const auto respond = [owner, request, generation](bool allowed) { if (owner && owner->worker_) owner->worker_->answerHost(request, generation, allowed); };
        if (answer.allowed) { owner->trustRequest_.clear(); respond(true); return; }
        if (!answer.result.ok || answer.canceled || !answer.inspection) {
            owner->trustRequest_.clear(); respond(false);
            owner->finish(false, answer.result.error.isEmpty() ? "The SSH server key could not be approved." : answer.result.error); return;
        }
        auto decide = [owner, request, generation, respond](SshHostKeyDecision decision) {
            if (!owner || !owner->trust_) return;
            if (decision == SshHostKeyDecision::Cancel) { owner->trust_->cancel(owner->trustRequest_); owner->trustRequest_.clear(); respond(false); return; }
            owner->trust_->decide(owner->trustRequest_, generation, decision, [owner, respond](SshHostKeyReply decided) {
                if (!owner) return;
                owner->trustRequest_.clear(); respond(decided.allowed);
                if (!decided.allowed) owner->finish(false, decided.result.error.isEmpty() ? "The SSH server key was not approved." : decided.result.error);
            });
        };
        auto* dialog = new SshHostKeyDialog(*answer.inspection, decide, owner); dialog->open();
    });
}

void SshTransferDialog::received(const QByteArray& bytes) {
    acks_ += bytes;
    while (!acks_.isEmpty() && step_ != Step::Idle) {
        const char code = acks_.at(0);
        if (code == 1 || code == 2) {
            const int end = acks_.indexOf('\n'); if (end < 0) return;
            finish(false, "The server refused the upload: " + QString::fromUtf8(acks_.mid(1, end - 1)).trimmed()); return;
        }
        if (code != 0) { finish(false, "The server did not answer as an SCP sink: " + QString::fromUtf8(acks_.left(200)).trimmed()); return; }
        acks_.remove(0, 1);
        if (step_ == Step::Ready) {
            step_ = Step::Header; status_->setText("Uploading " + QFileInfo(file_.fileName()).fileName() + "…");
            const auto header = "C0644 " + QByteArray::number(file_.size()) + " " + QFileInfo(file_.fileName()).fileName().toUtf8() + "\n";
            if (!worker_->input(header, generation_)) { finish(false, "Cannot send the upload header."); return; }
        } else if (step_ == Step::Header) { step_ = Step::Data; sendData(); }
        else if (step_ == Step::Final) { finish(true, "Uploaded " + QFileInfo(file_.fileName()).fileName() + "."); return; }
        else { finish(false, "Unexpected SCP acknowledgment."); return; }
    }
}

void SshTransferDialog::sendData() {
    if (step_ != Step::Data || !worker_) return;
    while (sent_ < file_.size()) {
        if (!file_.seek(sent_)) { finish(false, "Cannot read the local file."); return; }
        const auto chunk = file_.read(Chunk); if (chunk.isEmpty()) { finish(false, "Cannot read the local file."); return; }
        // A full input queue refuses the chunk; try again shortly.
        if (!worker_->input(chunk, generation_)) { retry_->start(); return; }
        sent_ += chunk.size(); progress_->setValue(int(sent_ * 100 / qMax<qint64>(file_.size(), 1)));
    }
    step_ = Step::Final;
    if (!worker_->input(QByteArray(1, '\0'), generation_)) { step_ = Step::Data; sent_ = file_.size(); retry_->start(); }
}

void SshTransferDialog::finish(bool ok, const QString& message) {
    retry_->stop(); step_ = Step::Idle; file_.close();
    if (worker_) { worker_->stop(); worker_ = nullptr; }
    if (ok) progress_->setValue(100);
    status_->setText(message); transfer_->setEnabled(true);
    emit finished(ok, message);
}
