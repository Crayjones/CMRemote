#pragma once
#include "VncWorker.h"
#include <QMessageBox>
#include <functional>

// Asks whether to trust a VNC server certificate that neither the system nor an imported
// authority verified. Always trust pins it for this host and port.
class VncCertificateDialog final : public QMessageBox {
    Q_OBJECT
public:
    using Completion = std::function<void(VncWorker::CertificateTrust)>;
    VncCertificateDialog(const VncCertificateReview& review, Completion complete, QWidget* parent = nullptr);
private:
    bool completed_ = false;
};
