#pragma once
#include "AtomicFile.h"
#include "ConnectionXml.h"

struct XmlDocumentRead { FileResult result; bool exists = false; };

// Blocking, worker-owned XML storage. Candidates publish at file replacement;
// no master/key is retained here.
class DocumentStore {
public:
    explicit DocumentStore(QString path);
    QString path() const { return file_.path(); }
    using XmlEdit = std::function<bool(ConnectionXml&)>;
    XmlDocumentRead loadXml(const Secret* master = nullptr, const ConnectionXml::Canceled& canceled = {});
    const ConnectionXml* xml() const { return xml_ ? &*xml_ : nullptr; }
    QByteArray revision() const { return file_.revision(); }
    bool exists() const { return file_.exists(); }
    bool xmlCurrent() const { return xml_ && file_.unchanged(ConnectionXml::MaxFile); }
    FileResult authenticateXml(const Secret& master, const ConnectionXml::Canceled& canceled = {});
    FileResult reviewXmlPasswords(const Secret& master, const std::vector<PasswordAuthorization>& reviewed,
                                 const ConnectionXml::Canceled& canceled = {});
    FileResult saveXmlAs(AtomicFile destination, const Secret& master, const ConnectionXml::Canceled& canceled = {});
    FileResult updateXml(const Secret& master, const XmlEdit& edit = {}, const Secret* replacement = nullptr,
                         const ConnectionXml::Canceled& canceled = {});
    void setFault(AtomicFile::Fault fault) { file_.setFault(fault); }
    void setCommitGate(AtomicFile::CommitGate gate) { file_.setCommitGate(std::move(gate)); }
private:
    AtomicFile file_;
    std::optional<ConnectionXml> xml_;
};
