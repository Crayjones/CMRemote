#include "VncCertificateDialog.h"
#include <QPushButton>

namespace {
QString date(const QDateTime& value) { return value.isValid() ? value.toString("yyyy-MM-dd") : QString("unknown"); }
}
VncCertificateDialog::VncCertificateDialog(const VncCertificateReview& review, Completion complete, QWidget* parent) : QMessageBox(parent) {
    const bool changed = review.previous.has_value();
    const auto& certificate = review.certificate;
    setObjectName("vncCertificateDialog");
    setWindowTitle(changed ? "VNC server certificate changed" : "Unverified VNC server certificate"); setTextFormat(Qt::PlainText);
    setIcon(changed ? QMessageBox::Warning : QMessageBox::Question);
    const auto endpoint = (review.host.contains(':') ? '[' + review.host + ']' : review.host) + ':' + QString::number(review.port);
    QString message = changed ?
        "The certificate of " + endpoint + " is not the one you chose to trust. The server may have been replaced or the connection may be intercepted.\n\n" :
        "The identity of " + endpoint + " could not be verified" + (review.reason.isEmpty() ? QString(".") : ": " + review.reason) + "\n\n";
    message += "Issued to: " + certificate.subject + "\nIssued by: " + certificate.issuer +
        "\nValid: " + date(certificate.notBefore) + " to " + date(certificate.notAfter) + "\nSHA-256: " + certificate.fingerprint();
    if (changed) message += "\n\nPreviously trusted SHA-256: " + review.previous->fingerprint();
    message += "\n\nCompare the fingerprint with the server's before trusting it. To trust every server of a private "
               "certificate authority, import the authority under Settings → Remote Desktop.";
    setText(message);
    auto* always = addButton(changed ? "Replace and always trust" : "Always trust", QMessageBox::AcceptRole); always->setObjectName("vncTrustAlways");
    auto* once = addButton("Connect once", QMessageBox::ActionRole); once->setObjectName("vncTrustOnce");
    auto* cancel = addButton("Cancel", QMessageBox::RejectRole); cancel->setObjectName("vncTrustCancel");
    setDefaultButton(cancel); setEscapeButton(cancel);
    setWindowModality(Qt::WindowModal); setAttribute(Qt::WA_DeleteOnClose);
    connect(this, &QDialog::finished, this, [this, complete = std::move(complete), always, once](int) {
        if (completed_) return;
        completed_ = true; const auto* chosen = clickedButton();
        complete(chosen == always ? VncWorker::CertificateTrust::Always : chosen == once ? VncWorker::CertificateTrust::Once : VncWorker::CertificateTrust::Reject);
    });
}
