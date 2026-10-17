#include "MainWindow.h"
#include "ConnectionImport.h"
#include "ExportDialog.h"
#include "ProfileEditor.h"
#include "ui_test.h"
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QUuid>

// mRemoteNG Import/Export. Every import format becomes mRemoteNG XML first (ConnectionImport).
QString MainWindow::importDestination() const {
    const auto id = selectedId(); if (id.isEmpty()) return {};
    const auto profile = profiles_.value(id);
    return profile.value("kind") == "folder" ? id : profile.value("parent").toString();
}

void MainWindow::showImportDialog() {
    const auto path = QFileDialog::getOpenFileName(this, "Import from File", {}, ConnectionImport::fileFilter());
    if (!path.isEmpty()) importConnections(path);
}

void MainWindow::importConnections(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > ConnectionXml::MaxFile) { fail("Cannot read " + QDir::toNativeSeparators(path) + "."); return; }
    QString error; const auto bytes = ConnectionImport::fromFile(path, file.readAll(), &error);
    if (!bytes) { fail(error.isEmpty() ? "Cannot import " + QFileInfo(path).fileName() + "." : error); return; }
    importXml(selectedDocument(), importDestination(), *bytes, QFileInfo(path).fileName());
}

void MainWindow::importPuttySessions() {
    QString error; const auto bytes = ConnectionImport::fromPuttySessions(&error);
    if (!bytes) { fail(error); return; }
    importXml(selectedDocument(), importDestination(), *bytes, "PuTTY sessions");
}

void MainWindow::importXml(DocumentId owner, QString destination, QByteArray bytes, QString label) {
    auto xml = ConnectionXml::read(bytes);
    if (!xml) { fail(label + " is not a supported connection file."); return; }
    Secret key(QByteArray("mR3m"));
    if (xml->unlock(key)) { finishImport(owner, destination, *xml, key, label); return; }
    // A password-protected mRemoteNG file: ask for its password, as mRemoteNG does.
    auto* dialog = new QInputDialog(this); dialog->setObjectName("importPassword");
    dialog->setWindowTitle("Import from File"); dialog->setLabelText("Password for " + label + ":");
    dialog->setTextEchoMode(QLineEdit::Password); dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, dialog, owner, destination, bytes, label] {
        Secret password(dialog->textValue().toUtf8()); dialog->setTextValue({});
        auto protectedXml = ConnectionXml::read(bytes);
        if (!protectedXml || !protectedXml->unlock(password)) { fail("The password for " + label + " is not correct."); return; }
        finishImport(owner, destination, *protectedXml, password, label);
    });
    dialog->open();
}

void MainWindow::finishImport(DocumentId owner, QString destination, ConnectionXml& xml, const Secret& key, QString label) {
    const auto projection = xml.projection(); auto passwords = xml.importPasswords(key);
    if (!projection || !passwords) { fail("Cannot read the connections in " + label + "."); return; }
    // New ids keep imported profiles distinct from existing ones, including a re-import of the same file.
    QHash<QString, QString> ids; QJsonArray batch; QStringList consumers;
    auto imported = std::make_shared<ImportedPasswords>();
    for (const auto& value : QJsonDocument::fromJson(*projection).object().value("profiles").toArray()) {
        const auto profile = value.toObject(); const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        ids.insert(profile.value("id").toString(), id);
        const auto parent = profile.value("parent").toString();
        const auto placed = parent.isEmpty() ? destination : ids.value(parent);
        batch.append(QJsonObject{{"id", id}, {"name", profile.value("name")}, {"kind", profile.value("kind")},
            {"parent", placed.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(placed)}, {"overrides", profile.value("overrides")}});
        if (profile.value("kind") == "connection") consumers << id;
    }
    if (batch.isEmpty()) { fail(label + " contains no connections."); return; }
    for (auto& [old, secret] : passwords->local) if (ids.contains(old)) imported->local.emplace(ids.value(old), std::move(secret));
    for (const auto& old : passwords->inherited) if (ids.contains(old)) imported->inherited.insert(ids.value(old));
    const auto count = batch.size();
    guard([this, owner, destination, batch, consumers, imported, label, count] {
        if (!(destination.isEmpty() ? selectConnectionFile(owner) : selectConnectionProfile({owner, destination}))) return;
        const auto doc = context(owner);
        if (!doc || !doc->canWrite()) { fail("Unlock this connection file before importing."); return; }
        try {
            const auto candidate = doc->prepareProfile(rm::ProfileChange::Import, QJsonDocument(batch).toJson(QJsonDocument::Compact));
            updateActions();
            files_->importProfiles(owner, candidate.projection, imported, consumers, candidate.reservation,
                [this, profile = candidate.profile, label, count](DocumentReply reply) {
                    const bool ok = reply.result.ok;
                    fileReply(std::move(reply), profile, {});
                    if (ok) notification(QString("Imported %1 item%2 from %3").arg(count).arg(count == 1 ? "" : "s").arg(label));
                });
        } catch (const std::exception&) { fail("Cannot import " + label + " into this connection file."); }
    });
}

void MainWindow::showExportDialog() {
    const auto owner = selectedDocument(); const auto id = selectedId(); const auto profile = profiles_.value(id);
    const auto folder = profile.value("kind") == "folder" ? id : profile.value("parent").toString();
    const auto connection = profile.value("kind") == "connection" ? id : QString();
    auto* dialog = new ExportDialog(folder, profiles_.value(folder).value("name").toString(), connection,
                                    profiles_.value(connection).value("name").toString(), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, dialog, owner] {
        const auto path = dialog->path(); const auto options = dialog->options();
        guard([this, owner, path, options] { if (selectConnectionFile(owner)) exportConnections(path, options); });
    });
    dialog->open();
}

void MainWindow::exportConnections(const QString& path, const ExportOptions& options) {
    const auto owner = selectedDocument(); const auto doc = context(owner); if (!doc) return;
    auto run = [this, owner, path, options] {
        files_->exportFile(owner, path, options, [this, path](DocumentReply reply) {
            if (!reply.result.ok) fail(reply.result.error);
            else notification("Exported to " + QDir::toNativeSeparators(path));
        });
    };
    // Exported passwords are decrypted from this file, so it must be unlocked.
    if (!doc->status().unlocked) promptFileUnlock(owner, run); else run();
}
