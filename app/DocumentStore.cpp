#include "DocumentStore.h"

namespace {
FileResult error(const char* text) { return {false, QString::fromLatin1(text), {}}; }
}
DocumentStore::DocumentStore(QString path) : file_(std::move(path)) {}
XmlDocumentRead DocumentStore::loadXml(const Secret* master, const ConnectionXml::Canceled& canceled) {
    XmlDocumentRead result;
    if (canceled && canceled()) { result.result = error("Connection file open canceled."); return result; }
    // Failed authentication/parsing must not adopt a revision belonging to bytes
    // that the retained XML representation never validated.
    auto inspected = file_; QByteArray bytes;
    if (!inspected.load(bytes, result.exists, ConnectionXml::MaxFile)) {
        result.result = error("Cannot read connection XML: unsafe path, read failure or size limit exceeded."); return result;
    }
    auto candidate = result.exists ? ConnectionXml::read(bytes, master, canceled) : ConnectionXml::create(canceled);
    if (!candidate || (!result.exists && master && !candidate->unlock(*master, canceled)) || (canceled && canceled())) {
        result.result = error("Cannot open connection XML: unsupported XML/crypto parameters, malformed data, invalid file master or canceled operation."); return result;
    }
    file_ = std::move(inspected); xml_ = std::move(candidate);
    result.result.ok = true; return result;
}
FileResult DocumentStore::authenticateXml(const Secret& master, const ConnectionXml::Canceled& canceled) {
    if (!xmlCurrent()) return error("Connection XML changed or is not open. Reopen it before unlocking.");
    auto candidate = xml_->clone();
    if (!candidate.unlock(master, canceled) || (canceled && canceled())) return error("Invalid file master or canceled unlock.");
    if (!file_.unchanged(ConnectionXml::MaxFile)) return error("Connection XML changed during unlock. Reopen it before continuing.");
    xml_ = std::move(candidate); return {true, {}, {}};
}
FileResult DocumentStore::reviewXmlPasswords(const Secret& master, const std::vector<PasswordAuthorization>& reviewed, const ConnectionXml::Canceled& canceled) {
    if (!xmlCurrent()) return error("Connection XML changed or is not open. Reopen it before reviewing passwords.");
    auto candidate = xml_->clone();
    if (!candidate.unlock(master, canceled) || !candidate.reviewPasswords(master, reviewed, canceled) || (canceled && canceled()))
        return error("Password review failed or was canceled. No file was changed.");
    if (!file_.unchanged(ConnectionXml::MaxFile)) return error("Connection XML changed during password review. Reopen it before continuing.");
    // Consent changes only the owned encrypted candidate. The inspected disk
    // revision stays unchanged until a later explicit XML replacement.
    xml_ = std::move(candidate); return {true, {}, {}};
}
FileResult DocumentStore::saveXmlAs(AtomicFile destination, const Secret& master, const ConnectionXml::Canceled& canceled) {
    if (!xmlCurrent()) return error("Connection XML changed or is not open. Reopen it before saving.");
    if (destination.path() == file_.path()) return error("Choose a different Save As destination.");
    auto candidate = xml_->clone();
    if (!candidate.unlock(master, canceled)) return error("Connection XML authentication failed or operation canceled.");
    const auto output = candidate.serialize(master, canceled);
    if (!output) return error("Connection XML validation/encryption failed or operation canceled.");
    const auto result = destination.replace(*output, ConnectionXml::MaxFile, "Connections", [&] {
        return (!canceled || !canceled()) && file_.unchanged(ConnectionXml::MaxFile);
    });
    if (result.ok) { file_ = std::move(destination); xml_ = std::move(candidate); }
    return result;
}
FileResult DocumentStore::updateXml(const Secret& master, const XmlEdit& edit, const Secret* replacement, const ConnectionXml::Canceled& canceled) {
    if (!xml_) return error("Connection XML is not open.");
    if (canceled && canceled()) return error("Connection XML save canceled.");
    if (!file_.unchanged(ConnectionXml::MaxFile)) return error("Connection XML changed outside this window. Reopen it before saving.");
    auto candidate = xml_->clone();
    if (!candidate.unlock(master, canceled)) return error("Connection XML authentication failed or operation canceled. Original file was not changed.");
    if ((canceled && canceled()) || (edit && !edit(candidate))) return error("Connection XML edit failed or was canceled. Original file was not changed.");
    const auto output = candidate.serialize(replacement ? *replacement : master, canceled);
    if (!output) return error("Connection XML validation/encryption failed or operation canceled. Original file was not changed.");
    const auto result = file_.replace(*output, ConnectionXml::MaxFile, "Connections", [&] { return !canceled || !canceled(); });
    // No cancellation test here: replacement has already committed, including
    // directory-sync warnings. Terminal adoption belongs to this store alone.
    if (result.ok) { xml_ = std::move(candidate); }
    return result;
}
