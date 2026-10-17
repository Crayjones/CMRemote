#include "MainWindow.h"
#include <QDir>
#include <QJsonDocument>

QString MainWindow::documentHelp() const {
    const auto doc = context(editorDocument_); if (!doc) return "Open or create a connection file.";
    const auto& status = doc->status();
    QString path = QDir::toNativeSeparators(status.path);
    const QString home = QDir::toNativeSeparators(QDir::homePath());
    if (path.startsWith(home + QDir::separator())) path.replace(0, home.size(), "~");
    return path + (status.unlocked ? " — passwords unlocked" : " — passwords locked");
}
void MainWindow::retryDocumentUnlock() { promptFileUnlock(selectedDocument()); }
void MainWindow::seedDemo() {
    if (documentState_ != DocumentState::Open || documentExists_ || protectedDocument_ || !profiles_.isEmpty()) return;
    const QJsonObject folder{{"name", "Examples (mock sessions)"}, {"kind", "folder"}, {"parent", QJsonValue(QJsonValue::Null)}, {"overrides", QJsonObject{}}};
    writeProfile(rm::ProfileChange::Create, QString::fromUtf8(QJsonDocument(folder).toJson(QJsonDocument::Compact)), [this] {
        const QJsonObject connection{{"name", "RDP mock"}, {"kind", "connection"}, {"parent", selectedId()}, {"overrides", QJsonObject{}}};
        writeProfile(rm::ProfileChange::Create, QString::fromUtf8(QJsonDocument(connection).toJson(QJsonDocument::Compact)));
    });
}
void MainWindow::writeProfile(rm::ProfileChange change, const QString& input, std::function<void()> next) {
    writeFileProfile(change, input, std::move(next));
}
