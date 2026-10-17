#include "MainWindow.h"
#include "SessionPane.h"
#include "GridDriver.h"
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>

namespace {
const QString Profile = "e6683f1f-4e06-4200-9a17-89cd30c68b12";
Secret master() { return Secret(QByteArray("synthetic document master")); }
QByteArray contents(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
bool seed(const QString& path, bool full) {
    const Secret initial(QByteArray("mR3m")); DocumentStore store(path);
    if (!store.loadXml().result.ok) return false;
    const auto projection = QJsonDocument(QJsonObject{{"schema_version", 2}, {"profiles", QJsonArray{QJsonObject{
        {"id", Profile}, {"name", "Private connection"}, {"kind", "connection"}, {"parent", QJsonValue()},
        {"overrides", QJsonObject{{"host", "localhost"}, {"username", "private-account"}, {"protocol", "rdp"}, {"port", 3389}}}}}}}).toJson(QJsonDocument::Compact);
    const auto replacement = master();
    return store.updateXml(initial, [&](ConnectionXml& xml) {
        return xml.patch(initial, projection).has_value() && (!full || (xml.setFullEncryption(initial, true) && xml.changeMaster(initial, replacement)));
    }, full ? &replacement : nullptr).ok;
}
QTreeWidgetItem* profile(MainWindow& window, const QString& id = Profile) {
    for (QTreeWidgetItemIterator it(window.findChild<QTreeWidget*>("connectionTree")); *it; ++it)
        if (treeProfileId(*it) == id) return *it;
    return nullptr;
}
void acceptMaster(QDialog* dialog) {
    dialog->findChild<QLineEdit*>("masterEdit")->setText("synthetic document master");
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
}
}
class DocumentShellTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qApp->setQuitOnLastWindowClosed(false); }
    void fullXmlSettingsSaveIsAtomicAndPreservesIds() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); QVERIFY(seed(path, true)); const auto original = contents(path);
        {
            MainWindow window(path, true); window.show(); QTRY_VERIFY(window.findChild<QDialog*>("filePassword")); QVERIFY(!profile(window));
            acceptMaster(window.findChild<QDialog*>("filePassword")); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open); QCOMPARE(contents(path), original);
            window.findChild<QTreeWidget*>("connectionTree")->setCurrentItem(profile(window)); QVERIFY(setDocumentFault(window, AtomicFile::Fault::BeforeReplace));
            setGridText(window, "domain", "EXAMPLE"); QTRY_VERIFY(!window.lastError().isEmpty()); QCOMPARE(contents(path), original);
            QVERIFY(window.findChild<ProfileEditor*>()->dirty()); QCOMPARE(gridValue(window, "domain").toString(), QString("EXAMPLE"));
            QVERIFY(setDocumentFault(window, AtomicFile::Fault::None)); window.saveSelected(); QTRY_VERIFY(window.findChild<QMessageBox*>("saveRecovery"));
            window.findChild<QMessageBox*>("saveRecovery")->button(QMessageBox::Retry)->click(); QVERIFY(waitGridSaved(window));
            QVERIFY(contents(path) != original); QVERIFY(!contents(path).contains("EXAMPLE"));
            QCOMPARE(storedXml(path, master())["profiles"].toArray().first().toObject()["id"].toString(), Profile);
        }
        const auto saved = contents(path); MainWindow restart(path, true); restart.show(); QTRY_VERIFY(restart.findChild<QDialog*>("filePassword"));
        acceptMaster(restart.findChild<QDialog*>("filePassword")); QTRY_COMPARE(restart.documentState(), MainWindow::DocumentState::Open); QCOMPARE(contents(path), saved);
        QVERIFY(profile(restart)); restart.findChild<QTreeWidget*>("connectionTree")->setCurrentItem(profile(restart)); QCOMPARE(gridValue(restart, "domain").toString(), QString("EXAMPLE"));
    }
    void fullEncryptionConfirmationUsesCurrentKey() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); QVERIFY(seed(path, false)); const auto original = contents(path);
        MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        setGridChoice(window, "full_encryption", true); QTRY_VERIFY(window.findChild<QMessageBox*>("confirmProtection"));
        auto* question = window.findChild<QMessageBox*>("confirmProtection"); QCOMPARE(question->defaultButton(), question->button(QMessageBox::Cancel));
        QVERIFY(question->text().contains("mR3m")); QCOMPARE(contents(path), original); question->button(QMessageBox::Yes)->click();
        QTRY_VERIFY(window.documentProtected()); QVERIFY(waitGridSaved(window)); QVERIFY(!window.findChild<QDialog*>("filePassword"));
        QVERIFY(!contents(path).contains("Private connection")); const Secret defaultMaster(QByteArray("mR3m")); auto xml = ConnectionXml::read(contents(path), &defaultMaster);
        QVERIFY(xml && xml->fullEncryption()); QVERIFY(xml->passwordProtected() == false);
        setGridChoice(window, "full_encryption", false); QTRY_VERIFY(window.findChild<QMessageBox*>("confirmProtection"));
        window.findChild<QMessageBox*>("confirmProtection")->button(QMessageBox::Yes)->click(); QTRY_VERIFY(!window.documentProtected()); QVERIFY(waitGridSaved(window));
        QVERIFY(contents(path).contains("Private connection"));
    }
    void canceledFullEncryptionRestoresCommittedMode() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); QVERIFY(seed(path, false)); const auto original = contents(path);
        MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        setGridChoice(window, "full_encryption", true); window.close(); QTRY_VERIFY(window.findChild<QMessageBox*>("confirmProtection"));
        window.findChild<QMessageBox*>("confirmProtection")->button(QMessageBox::Cancel)->click();
        QTRY_VERIFY(!window.findChild<ProfileEditor*>()->dirty()); QVERIFY(window.isVisible()); QVERIFY(!window.documentProtected()); QCOMPARE(contents(path), original);
    }
    void rootReplacementWarningWaitsForSelection() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); QVERIFY(seed(path, false)); MainWindow window(path, true); window.show();
        QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open); QVERIFY(setDocumentFault(window, AtomicFile::Fault::DirectorySync));
        setGridChoice(window, "full_encryption", true); window.findChild<QTreeWidget*>("connectionTree")->setCurrentItem(profile(window));
        QTRY_VERIFY(window.findChild<QMessageBox*>("confirmProtection")); QVERIFY(window.documentRootSelected());
        window.findChild<QMessageBox*>("confirmProtection")->button(QMessageBox::Yes)->click(); QTRY_VERIFY(window.findChild<QMessageBox*>("durabilityWarning"));
        QVERIFY(window.documentProtected()); QVERIFY(window.documentRootSelected()); QCOMPARE(gridValue(window, "full_encryption").toBool(), true);
        window.findChild<QMessageBox*>("durabilityWarning")->button(QMessageBox::Ok)->click(); QTRY_COMPARE(window.selectedId(), Profile);
    }
    void rootLockOnEitherSideOfReplacement_data() { QTest::addColumn<bool>("after"); QTest::newRow("before") << false; QTest::newRow("after") << true; }
    void rootLockOnEitherSideOfReplacement() {
        QFETCH(bool, after); QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); QVERIFY(seed(path, false)); const auto original = contents(path);
        MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        XmlCommitBarrier barrier; QVERIFY(setDocumentGate(window, barrier.gate(after)));
        setGridChoice(window, "full_encryption", true); QTRY_VERIFY(window.findChild<QMessageBox*>("confirmProtection"));
        window.findChild<QMessageBox*>("confirmProtection")->button(QMessageBox::Yes)->click(); QTRY_VERIFY(barrier.entered());
        window.close(); window.findChild<DocumentService*>()->lock(window.selectedDocument()); barrier.release(); QTRY_VERIFY(!documentBusy(window));
        QVERIFY(window.isVisible()); QCOMPARE(window.documentProtected(), after);
        if (after) { QVERIFY(!window.findChild<ProfileEditor*>()->dirty()); QVERIFY(contents(path) != original); }
        else { QCOMPARE(contents(path), original); QVERIFY(window.findChild<ProfileEditor*>()->dirty()); }
    }
    void rootExternalRevisionConflictPreservesReplacement() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); QVERIFY(seed(path, false)); MainWindow window(path, true); window.show();
        QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open); setGridChoice(window, "full_encryption", true); QTRY_VERIFY(window.findChild<QMessageBox*>("confirmProtection"));
        const auto external = contents(path) + '\n'; QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(external), external.size()); file.close();
        window.findChild<QMessageBox*>("confirmProtection")->button(QMessageBox::Yes)->click(); QTRY_VERIFY(!window.lastError().isEmpty());
        QCOMPARE(contents(path), external); QVERIFY(!window.documentProtected()); QVERIFY(window.findChild<ProfileEditor*>()->dirty());
    }
    void protectedSaveAsAndCrudSurviveRestart() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); QVERIFY(seed(path, true)); QString savedId;
        {
            MainWindow window(path, true); window.show(); QTRY_VERIFY(window.findChild<QDialog*>("filePassword")); acceptMaster(window.findChild<QDialog*>("filePassword"));
            QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open); window.quickConnect("localhost:3391", true); window.saveAsConnection();
            QTRY_VERIFY(window.findChild<QDialog*>("saveAsConnection")); auto* dialog = window.findChild<QDialog*>("saveAsConnection");
            dialog->findChild<QLineEdit*>("connectionName")->setText("Protected saved target"); dialog->accept();
            QTRY_VERIFY(!window.selectedId().isEmpty()); QVERIFY(waitGridSaved(window)); closeGridEditors(window); savedId = window.selectedId();
            setGridText(window, "name", "Protected renamed target"); QVERIFY(waitGridSaved(window)); window.addFolder(); QVERIFY(waitGridSaved(window)); closeGridEditors(window);
            window.deleteSelected(); QVERIFY(waitGridSaved(window)); window.close(); QTRY_VERIFY(!window.isVisible()); QVERIFY(!contents(path).contains("Protected renamed target"));
        }
        MainWindow restart(path, true); restart.show(); QTRY_VERIFY(restart.findChild<QDialog*>("filePassword")); QVERIFY(!profile(restart, savedId));
        acceptMaster(restart.findChild<QDialog*>("filePassword")); QTRY_COMPARE(restart.documentState(), MainWindow::DocumentState::Open);
        QVERIFY(profile(restart, savedId)); QCOMPARE(profile(restart, savedId)->text(0), QString("Protected renamed target")); QVERIFY(profile(restart));
    }
    void protectedStartupRequiresMaster() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); QVERIFY(seed(path, true)); MainWindow window(path, true); window.show();
        QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Locked); QVERIFY(!profile(window)); QVERIFY(window.documentRootSelected());
        QVERIFY(!window.findChild<QAction*>("actionAddConnection")->isEnabled()); QVERIFY(!(gridRow(window, "full_encryption")->flags() & Qt::ItemIsEditable));
        QTRY_VERIFY(window.findChild<QDialog*>("filePassword")); acceptMaster(window.findChild<QDialog*>("filePassword"));
        QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open); QVERIFY(profile(window)); QVERIFY(window.documentProtected());
    }
    void canceledDocumentUnlockNeverExposesProfiles() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); QVERIFY(seed(path, true)); const auto original = contents(path);
        MainWindow window(path, true); window.show(); QTRY_VERIFY(window.findChild<QDialog*>("filePassword")); window.findChild<QDialog*>("filePassword")->reject();
        QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Locked); QVERIFY(!profile(window)); QTRY_VERIFY(!window.findChild<QDialog*>("filePassword"));
        window.retryDocumentUnlock(); QTRY_VERIFY(window.findChild<QDialog*>("filePassword")); auto* dialog = window.findChild<QDialog*>("filePassword");
        dialog->findChild<QLineEdit*>("masterEdit")->setText("wrong synthetic master"); dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        QTRY_VERIFY(window.findChild<QDialog*>("filePassword") &&
            !window.findChild<QDialog*>("filePassword")->findChild<QLabel*>("errorLabel")->text().isEmpty());
        QVERIFY(window.findChild<QDialog*>("filePassword")->findChild<QLabel*>("hintLabel")->text().contains("Attempt 2 of 3"));
        QVERIFY(!profile(window)); QCOMPARE(contents(path), original);
    }
    void documentSyncWarningBlocksTransition() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); QVERIFY(seed(path, false)); MainWindow window(path, true); window.show();
        QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open); window.findChild<QTreeWidget*>("connectionTree")->setCurrentItem(profile(window));
        QVERIFY(setDocumentFault(window, AtomicFile::Fault::DirectorySync)); setGridText(window, "name", "Saved before connect"); window.connectSelected(true);
        QTRY_VERIFY(window.findChild<QMessageBox*>("durabilityWarning")); QCOMPARE(window.sessionCount(), size_t(0)); QVERIFY(contents(path).contains("Saved before connect"));
        window.findChild<QMessageBox*>("durabilityWarning")->button(QMessageBox::Ok)->click(); QTRY_COMPARE(window.sessionCount(), size_t(1));
    }
    void lockClearsRetainedMasterWithoutDisconnect() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); QVERIFY(seed(path, true)); MainWindow window(path, true); window.show();
        QTRY_VERIFY(window.findChild<QDialog*>("filePassword")); acceptMaster(window.findChild<QDialog*>("filePassword")); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        window.findChild<QTreeWidget*>("connectionTree")->setCurrentItem(profile(window)); window.connectSelected(true); QTRY_COMPARE(window.sessionCount(), size_t(1));
        auto* pane = qobject_cast<SessionPane*>(window.findChild<QTabWidget*>("sessionTabs")->widget(0)); QVERIFY(pane && pane->active());
        auto* service = window.findChild<DocumentService*>(); const auto id = window.selectedDocument(); service->lock(id); QVERIFY(!service->status(id)->unlocked);
        QVERIFY(pane->active()); QVERIFY(profile(window)); auto* config = window.findChild<QTreeWidget*>("configTree");
        QTRY_VERIFY(!config->isEnabled()); config->setCurrentItem(gridRow(window, "name"), 1);
        QTest::keyClick(config, Qt::Key_Return); QVERIFY(!qobject_cast<PropertyDelegate*>(config->itemDelegate())->activeEditor());
        const auto original = contents(path); std::optional<DocumentReply> reply;
        service->save(id, QJsonDocument(storedXml(path, master())).toJson(QJsonDocument::Compact), "locked", [&](DocumentReply value) { reply = std::move(value); });
        QTRY_VERIFY(reply.has_value()); QVERIFY(!reply->result.ok && !reply->committed); QCOMPARE(contents(path), original); QVERIFY(pane->active());
    }
};
QTEST_MAIN(DocumentShellTest)
#include "document_shell.moc"
