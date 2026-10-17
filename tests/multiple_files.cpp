#include "MainWindow.h"
#include "SessionPane.h"
#include "ConnectionXml.h"
#include "ProfileEditor.h"
#include "ConnectionTree.h"
#include "GridDriver.h"
#include <QMessageBox>
#include <QLabel>
#include <QTabWidget>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QAction>
#include <QContextMenuEvent>
#include <QMenu>
#include <QPersistentModelIndex>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QSemaphore>
#include <QScrollBar>
#include <QTreeWidgetItemIterator>

namespace {
const QString Profile = "e6683f1f-4e06-4200-9a17-89cd30c68b12";
QByteArray contents(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
bool fixture(const QString& path, const QString& host, const QByteArray& custom = {}, bool full = false) {
    auto xml = ConnectionXml::create(); Secret master(QByteArray("mR3m"));
    const QJsonObject profile{{"id", Profile}, {"name", "Copied UUID"}, {"kind", "connection"}, {"parent", QJsonValue()},
        {"overrides", QJsonObject{{"host", host}, {"username", "synthetic-user"}, {"protocol", "rdp"}, {"port", 3389}}}};
    const auto projection = QJsonDocument(QJsonObject{{"schema_version", 2}, {"profiles", QJsonArray{profile}}}).toJson(QJsonDocument::Compact);
    if (!xml || !xml->unlock(master) || !xml->patch(master, projection)) return false;
    if (!custom.isEmpty()) { Secret replacement(custom); if (!xml->changeMaster(master, replacement)) return false; master = std::move(replacement); }
    if (!xml->setFullEncryption(master, full)) return false;
    const auto output = xml->serialize(master); QFile f(path);
    return output && f.open(QIODevice::WriteOnly) && f.write(*output) == output->size();
}
QTreeWidgetItem* item(ConnectionTree* tree, ProfileReference ref) {
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->type() == ConnectionTree::ProfileItem && (*it)->data(0, Qt::UserRole).value<ProfileReference>() == ref) return *it;
    return nullptr;
}
bool drop(ConnectionTree* tree, QMimeData* mime, QTreeWidgetItem* target) {
    if (!mime || !target) return false;
    tree->scrollToItem(target); const auto point = tree->visualItemRect(target).center();
    QDragEnterEvent enter(QPointF(point), Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(tree->viewport(), &enter);
    QDragMoveEvent move(QPointF(point), Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(tree->viewport(), &move);
    QDropEvent event(point, Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(tree->viewport(), &event);
    return event.isAccepted();
}
struct Barrier {
    QSemaphore entered, release;
    bool after = false;
    AtomicFile::CommitGate gate() {
        return [this](const auto& commit) {
            bool result = false; if (after) result = commit();
            entered.release(); release.acquire(); return after ? result : commit();
        };
    }
};
struct Unblock {
    Barrier& barrier;
    ~Unblock() { barrier.release.release(); }
};
}
class MultipleFilesTest : public QObject {
    Q_OBJECT
private slots:
    void xmlAutoSavePreservesConfigViewport() {
        QTemporaryDir dir; const auto path = dir.filePath("viewport.xml"); QVERIFY(fixture(path, "127.0.0.1"));
        MainWindow window(path, true); window.resize(800, 480); window.show();
        auto* service = window.findChild<DocumentService*>(); const auto id = window.connectionFiles().first();
        QTRY_VERIFY(service->status(id) && !service->status(id)->busy);
        QVERIFY(window.selectConnectionProfile({id, Profile}));
        auto* grid = window.findChild<QTreeWidget*>("configTree");
        auto* row = gridRow(window, "font_smoothing");
        auto* choice = qobject_cast<QComboBox*>(gridEditor(window, "font_smoothing")); QVERIFY(choice);
        const auto scroll = grid->verticalScrollBar()->value(); QVERIFY(scroll > 0);
        const auto expanded = row->parent()->isExpanded();
        QTest::keyClick(choice, Qt::Key_Home); QTest::keyClick(choice, Qt::Key_Return);
        QTRY_VERIFY(!window.findChild<ProfileEditor*>()->dirty() && !service->status(id)->busy);
        QTest::qWait(80);
        QCOMPARE(gridRow(window, "font_smoothing"), row); QCOMPARE(grid->currentItem(), row);
        QCOMPARE(grid->verticalScrollBar()->value(), scroll); QCOMPARE(row->parent()->isExpanded(), expanded);
        bool ready = false; service->setFault(id, AtomicFile::Fault::DirectorySync, [&](DocumentReply reply) { ready = reply.result.ok; }); QTRY_VERIFY(ready);
        choice = qobject_cast<QComboBox*>(gridEditor(window, "font_smoothing")); QVERIFY(choice);
        const auto warningScroll = grid->verticalScrollBar()->value();
        QTest::keyClick(choice, Qt::Key_End); QTest::keyClick(choice, Qt::Key_Return);
        QTRY_VERIFY(window.findChild<QMessageBox*>("durabilityWarning"));
        QCOMPARE(gridRow(window, "font_smoothing"), row); QCOMPARE(grid->currentItem(), row);
        QCOMPARE(grid->verticalScrollBar()->value(), warningScroll);
        window.findChild<QMessageBox*>("durabilityWarning")->done(QMessageBox::Ok);
    }
    void copiedIdsDoNotShareEditorState() {
        QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
        QVERIFY(fixture(a, "127.0.0.1") && fixture(b, "localhost"));
        MainWindow window(a, true); window.show(); auto* service = window.findChild<DocumentService*>();
        const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
        QTRY_VERIFY(service->status(idA) && !service->status(idA)->busy && service->status(idB) && !service->status(idB)->busy);
        QVERIFY(window.selectConnectionProfile({idA, Profile})); setGridText(window, "description", "A only");
        QTRY_VERIFY(!window.findChild<ProfileEditor*>()->dirty() && !service->status(idA)->busy);
        QVERIFY(window.selectConnectionProfile({idB, Profile})); QCOMPARE(gridValue(window, "host").toString(), QString("localhost"));
        QVERIFY(gridValue(window, "description").toString() != "A only");
        auto* grid = window.findChild<QTreeWidget*>("configTree"); auto* row = gridRow(window, "description");
        auto* edit = qobject_cast<QLineEdit*>(gridEditor(window, "description")); edit->setText("B active draft");
        bool refreshed = false; service->setFault(idA, AtomicFile::Fault::None, [&](DocumentReply) { refreshed = true; }); QTRY_VERIFY(refreshed);
        QCOMPARE(grid->currentItem(), row); QVERIFY(edit == qobject_cast<PropertyDelegate*>(grid->itemDelegate())->activeEditor()); QCOMPARE(edit->text(), QString("B active draft"));
        QTest::keyClick(edit, Qt::Key_Return); QTRY_VERIFY(!window.findChild<ProfileEditor*>()->dirty() && !service->status(idB)->busy);
        QVERIFY(window.selectConnectionProfile({idA, Profile})); QCOMPARE(gridValue(window, "description").toString(), QString("A only"));
        QVERIFY(window.selectConnectionProfile({idB, Profile})); QCOMPARE(gridValue(window, "description").toString(), QString("B active draft"));
    }
    void productionRootsAndCopiedIdsAreIndependent() {
        QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
        QVERIFY(fixture(a, "127.0.0.1") && fixture(b, "localhost")); const auto sourceB = contents(b);
        MainWindow window(a, true); window.show();
        QTRY_COMPARE(window.connectionFiles().size(), 1);
        const auto idA = window.connectionFiles().first(); const auto idB = window.openConnectionFile(b);
        QVERIFY(idA.valid() && idB.valid() && idA != idB);
        auto* tree = window.findChild<QTreeWidget*>("connectionTree"); QVERIFY(tree);
        QTRY_COMPARE(tree->topLevelItemCount(), 2);
        QTRY_COMPARE(tree->topLevelItem(0)->childCount(), 1);
        QTRY_COMPARE(tree->topLevelItem(1)->childCount(), 1);
        QCOMPARE(tree->topLevelItem(0)->data(0, Qt::UserRole).value<DocumentRootReference>().document, idA);
        QCOMPARE(tree->topLevelItem(1)->data(0, Qt::UserRole).value<DocumentRootReference>().document, idB);
        QVERIFY(tree->topLevelItem(0)->text(0).contains("a.xml")); QVERIFY(tree->topLevelItem(1)->text(0).contains("b.xml"));
        QVERIFY(window.selectConnectionProfile({idA, Profile})); window.connectSelected(true);
        QVERIFY(window.selectConnectionProfile({idB, Profile})); window.connectSelected(true);
        QCOMPARE(window.sessionCount(), size_t(2)); QCOMPARE(window.findChildren<SessionPane*>().size(), 2);
        const auto pages = window.findChildren<SessionPane*>(); QVERIFY(!(pages[0]->target() == pages[1]->target()));
        window.closeConnectionFile(); QCOMPARE(window.connectionFiles().size(), 2);
        QVERIFY(window.lastError().contains("tabs"));
        window.disconnectCurrent(); window.closeConnectionFile(); QCOMPARE(window.connectionFiles().size(), 2);
        window.closeTab(1); window.closeConnectionFile(); QTRY_COMPARE(window.connectionFiles().size(), 1);
        QCOMPARE(window.connectionFiles().first(), idA); QCOMPARE(contents(b), sourceB);
    }
    void configSavesAndReconnectResolveOwningFile() {
        QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
        QVERIFY(fixture(a, "127.0.0.1") && fixture(b, "localhost")); const auto sourceB = contents(b);
        MainWindow window(a, true); window.show();
        auto* service = window.findChild<DocumentService*>(); QVERIFY(service);
        const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
        QTRY_VERIFY(service->status(idA) && !service->status(idA)->busy && service->status(idB) && !service->status(idB)->busy);
        QVERIFY(window.selectConnectionProfile({idA, Profile})); window.connectSelected(true);
        auto* paneA = window.findChild<SessionPane*>(); QVERIFY(paneA); QCOMPARE(paneA->document(), idA);
        auto* tree = window.findChild<QTreeWidget*>("connectionTree"); QVERIFY(tree);
        const QPersistentModelIndex rootB(tree->model()->index(1, 0));
        const QPersistentModelIndex profileB(tree->model()->index(0, 0, rootB));
        QVERIFY(rootB.isValid() && profileB.isValid());
        setGridText(window, "host", "127.0.0.2");
        QVERIFY(window.selectConnectionProfile({idB, Profile}));
        QTRY_VERIFY(!window.findChild<ProfileEditor*>()->dirty() && !service->status(idA)->busy);
        QTRY_COMPARE(window.selectedDocument(), idB); QCOMPARE(contents(b), sourceB);
        QVERIFY(rootB.isValid() && profileB.isValid());
        QVERIFY(contents(a).contains("127.0.0.2")); QCOMPARE(paneA->target().host, QString("127.0.0.1"));
        window.reconnectCurrent(); QVERIFY2(window.lastError().isEmpty(), qPrintable(window.lastError())); QCOMPARE(window.sessionCount(), size_t(1));
        paneA = window.findChild<SessionPane*>(); QVERIFY(paneA); QCOMPARE(paneA->document(), idA); QCOMPARE(paneA->target().host, QString("127.0.0.2"));
        window.connectSelected(true); QCOMPARE(window.sessionCount(), size_t(2));
        const auto pages = window.findChildren<SessionPane*>();
        QVERIFY(pages[0]->document() != pages[1]->document());
        QVERIFY(window.selectConnectionFile(idA)); window.findChild<QAction*>("actionLockPasswords")->trigger();
        QVERIFY(!service->status(idA)->unlocked && service->status(idB)->unlocked); QCOMPARE(window.sessionCount(), size_t(2));
        window.closeConnectionFile(); QCOMPARE(window.connectionFiles().size(), 2);
    }
    void durabilityWarningGatesOnlyOwningRoot() {
        QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
        QVERIFY(fixture(a, "127.0.0.1") && fixture(b, "localhost"));
        MainWindow window(a, true); window.show(); auto* service = window.findChild<DocumentService*>(); QVERIFY(service);
        const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
        QTRY_VERIFY(service->status(idA) && !service->status(idA)->busy && service->status(idB) && !service->status(idB)->busy);
        bool faultReady = false; service->setFault(idA, AtomicFile::Fault::DirectorySync, [&](DocumentReply reply) { faultReady = reply.result.ok; }); QTRY_VERIFY(faultReady);
        QVERIFY(window.selectConnectionProfile({idA, Profile})); setGridText(window, "name", "Saved A with warning");
        QTRY_VERIFY(window.findChild<QMessageBox*>("durabilityWarning"));
        QVERIFY(!window.findChild<QMessageBox*>("durabilityWarning")->isModal());
        QVERIFY(!service->status(idA)->warning.isEmpty()); QVERIFY(contents(a).contains("Saved A with warning"));
        QVERIFY(window.selectConnectionProfile({idB, Profile})); QTRY_COMPARE(window.selectedDocument(), idB);
        QVERIFY(window.findChild<QAction*>("actionAddConnection")->isEnabled());
        setGridText(window, "name", "Independent B");
        QTRY_VERIFY(!window.findChild<ProfileEditor*>()->dirty() && !service->status(idB)->busy);
        QVERIFY(contents(b).contains("Independent B")); QVERIFY(!service->status(idA)->warning.isEmpty());
        window.findChild<QMessageBox*>("durabilityWarning")->reject();
        QTRY_VERIFY(!window.findChild<QMessageBox*>("durabilityWarning"));
        QVERIFY(!service->status(idA)->warning.isEmpty());
        QVERIFY(window.selectConnectionFile(idA));
        auto* tree = window.findChild<QTreeWidget*>("connectionTree"); QVERIFY(tree);
        QContextMenuEvent menuEvent(QContextMenuEvent::Keyboard, QPoint(), tree->mapToGlobal(tree->rect().center()));
        QApplication::sendEvent(tree, &menuEvent);
        auto* menu = window.findChild<QMenu*>("treeContextMenu"); QVERIFY(menu);
        auto* warningAction = menu->findChild<QAction*>("actionFileWarning"); QVERIFY(warningAction && warningAction->isEnabled());
        QVERIFY(window.selectConnectionProfile({idB, Profile}));
        warningAction->trigger(); menu->close();
        QTRY_VERIFY(window.findChild<QMessageBox*>("durabilityWarning"));
        QVERIFY(!window.findChild<QMessageBox*>("durabilityWarning")->isModal());
        window.findChild<QMessageBox*>("durabilityWarning")->done(QMessageBox::Ok);
        QTRY_VERIFY(service->status(idA)->warning.isEmpty()); QCOMPARE(window.selectedDocument(), idB);
    }
    void warningContinuationRetainsCapturedProfileAfterForeignEdit() {
        QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
        QVERIFY(fixture(a, "127.0.0.1") && fixture(b, "localhost"));
        MainWindow window(a, true); window.show(); auto* service = window.findChild<DocumentService*>(); QVERIFY(service);
        const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
        QTRY_VERIFY(service->status(idA) && !service->status(idA)->busy && service->status(idB) && !service->status(idB)->busy);
        bool ready = false; service->setFault(idA, AtomicFile::Fault::DirectorySync, [&](DocumentReply reply) { ready = reply.result.ok; }); QTRY_VERIFY(ready);
        QVERIFY(window.selectConnectionProfile({idA, Profile})); setGridText(window, "name", "A queued Connect"); window.connectSelected(true);
        QTRY_VERIFY(window.findChild<QMessageBox*>("durabilityWarning")); QCOMPARE(window.sessionCount(), size_t(0));
        QVERIFY(window.selectConnectionProfile({idB, Profile})); setGridText(window, "name", "B pending edit");
        window.findChild<QMessageBox*>("durabilityWarning")->done(QMessageBox::Ok);
        QTRY_COMPARE(window.sessionCount(), size_t(1));
        auto* pane = window.findChild<SessionPane*>(); QVERIFY(pane); QCOMPARE(pane->document(), idA);
        QTRY_VERIFY(!service->status(idB)->busy && !window.findChild<ProfileEditor*>()->dirty());
        QVERIFY(contents(b).contains("B pending edit")); QCOMPARE(window.selectedDocument(), idA);
    }
    void applicationCloseWaitsForEveryFilesWarning() {
        QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
        QVERIFY(fixture(a, "127.0.0.1") && fixture(b, "localhost"));
        MainWindow window(a, true); window.show(); auto* service = window.findChild<DocumentService*>(); QVERIFY(service);
        const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
        QTRY_VERIFY(service->status(idA) && !service->status(idA)->busy && service->status(idB) && !service->status(idB)->busy);
        bool ready = false; service->setFault(idA, AtomicFile::Fault::DirectorySync, [&](DocumentReply reply) { ready = reply.result.ok; }); QTRY_VERIFY(ready);
        QVERIFY(window.selectConnectionProfile({idA, Profile})); setGridText(window, "name", "A awaiting acknowledgement");
        QTRY_VERIFY(window.findChild<QMessageBox*>("durabilityWarning"));
        QVERIFY(window.selectConnectionProfile({idB, Profile})); setGridText(window, "name", "B saved before close");
        QVERIFY(!window.close());
        QTRY_VERIFY(!window.findChild<ProfileEditor*>()->dirty() && !service->status(idB)->busy);
        QVERIFY(contents(b).contains("B saved before close"));
        QVERIFY(window.isVisible()); QVERIFY(!service->status(idA)->warning.isEmpty());
        window.findChild<QMessageBox*>("durabilityWarning")->done(QMessageBox::Ok);
        QTRY_VERIFY(!window.isVisible());
    }
    void temporarySaveChoosesExplicitFileAndKeepsSessionIndependent() {
        QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
        QVERIFY(fixture(a, "127.0.0.1") && fixture(b, "localhost")); const auto sourceA = contents(a);
        MainWindow window(a, true); window.show(); auto* service = window.findChild<DocumentService*>(); QVERIFY(service);
        const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
        QTRY_VERIFY(service->status(idA) && !service->status(idA)->busy && service->status(idB) && !service->status(idB)->busy);
        QVERIFY(window.selectConnectionFile(idA));
        QVERIFY(!window.selectConnectionProfile({idA, {}})); QCOMPARE(window.selectedDocument(), idA);
        window.quickConnect("127.0.0.3:3391", true); auto* pane = window.findChild<SessionPane*>(); QVERIFY(pane);
        QVERIFY(!pane->document().valid()); const auto target = pane->target();
        window.saveAsConnection(); QTRY_VERIFY(window.findChild<QDialog*>("saveAsConnection"));
        auto* dialog = window.findChild<QDialog*>("saveAsConnection"); auto* file = dialog->findChild<QComboBox*>("connectionFile"); QVERIFY(file);
        QCOMPARE(file->count(), 2); const auto index = file->findData(QVariant::fromValue(idB)); QVERIFY(index >= 0); file->setCurrentIndex(index);
        dialog->findChild<QLineEdit*>("connectionName")->setText("Explicit B target");
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        QTRY_VERIFY(contents(b).contains("Explicit B target")); QTRY_VERIFY(!service->status(idB)->busy);
        QCOMPARE(contents(a), sourceA); QCOMPARE(pane->target(), target); QVERIFY(!pane->document().valid()); QCOMPARE(window.sessionCount(), size_t(1));
        window.closeConnectionFile(); QTRY_COMPARE(window.connectionFiles().size(), 1); QCOMPARE(window.sessionCount(), size_t(1));
    }
    void rootFullEncryptionIsOneAtomicReplacementAndDefaultReopenNeedsNoPrompt() {
        QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
        QVERIFY(fixture(a, "127.0.0.1") && fixture(b, "localhost")); const auto originalA = contents(a), originalB = contents(b);
        {
            MainWindow window(a, true); window.show(); auto* service = window.findChild<DocumentService*>(); QVERIFY(service);
            const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
            QTRY_VERIFY(service->status(idA) && !service->status(idA)->busy && service->status(idB) && !service->status(idB)->busy);
            bool ready = false; service->setFault(idA, AtomicFile::Fault::BeforeReplace, [&](DocumentReply reply) { ready = reply.result.ok; }); QTRY_VERIFY(ready);
            QVERIFY(window.selectConnectionFile(idA)); setGridChoice(window, "full_encryption", true);
            QTRY_VERIFY(window.findChild<QMessageBox*>("confirmProtection")); window.findChild<QMessageBox*>("confirmProtection")->button(QMessageBox::Yes)->click();
            QTRY_VERIFY(!service->status(idA)->busy && !window.lastError().isEmpty()); QVERIFY(!window.findChild<QDialog*>("filePassword"));
            QCOMPARE(contents(a), originalA); QCOMPARE(contents(b), originalB); QVERIFY(!service->status(idA)->fullEncryption && service->status(idA)->unlocked);
            QVERIFY(service->status(idA)->passwordProtected == false);
            window.saveSelected(); QTRY_VERIFY(window.findChild<QMessageBox*>("saveRecovery")); window.findChild<QMessageBox*>("saveRecovery")->done(QMessageBox::Discard);
            ready = false; service->setFault(idA, AtomicFile::Fault::None, [&](DocumentReply reply) { ready = reply.result.ok; }); QTRY_VERIFY(ready);
            setGridChoice(window, "full_encryption", true); QTRY_VERIFY(window.findChild<QMessageBox*>("confirmProtection")); window.findChild<QMessageBox*>("confirmProtection")->button(QMessageBox::Yes)->click();
            QTRY_VERIFY(service->status(idA)->fullEncryption && !service->status(idA)->busy && !window.findChild<ProfileEditor*>()->dirty());
            QVERIFY(service->status(idA)->passwordProtected == false); QVERIFY(!window.findChild<QDialog*>("filePassword"));
            QVERIFY(window.documentProtected()); QVERIFY(!contents(a).contains("127.0.0.1")); QCOMPARE(contents(b), originalB); QVERIFY(!QFile::exists(a + ".vault"));
        }
        MainWindow reopened(a, true); reopened.show(); auto* service = reopened.findChild<DocumentService*>(); QVERIFY(service);
        const auto idA = reopened.connectionFiles().first(), idB = reopened.openConnectionFile(b);
        QTRY_VERIFY(service->status(idA) && !service->status(idA)->busy && service->status(idB) && !service->status(idB)->busy);
        QVERIFY(service->status(idA)->unlocked && service->status(idA)->passwordProtected == false); QVERIFY(!reopened.findChild<QDialog*>("filePassword"));
        auto* tree = reopened.findChild<QTreeWidget*>("connectionTree"); QCOMPARE(tree->topLevelItem(0)->childCount(), 1); QCOMPARE(tree->topLevelItem(1)->childCount(), 1); QCOMPARE(contents(b), originalB);
    }
    void protectedXmlOpenPromptsOncePerFile_data() {
        QTest::addColumn<bool>("full"); QTest::newRow("password attributes") << false; QTest::newRow("full inner XML") << true;
    }
    void protectedXmlOpenPromptsOncePerFile() {
        QFETCH(bool, full); QTemporaryDir dir; const auto path = dir.filePath("protected.xml");
        QVERIFY(fixture(path, "127.0.0.1", "synthetic master", full)); const auto before = contents(path);
        MainWindow window(path, true); window.show(); auto* service = window.findChild<DocumentService*>(); const auto id = window.connectionFiles().first();
        QTRY_VERIFY(window.findChild<QDialog*>("filePassword")); auto* dialog = window.findChild<QDialog*>("filePassword");
        QCOMPARE(window.findChildren<QDialog*>("filePassword").size(), 1); QVERIFY(dialog->windowTitle().contains("protected.xml"));
        QVERIFY(dialog->findChild<QLabel*>("hintLabel")->text().contains(path)); QVERIFY(!service->status(id)->passwordProtected.has_value());
        dialog->findChild<QLineEdit*>("masterEdit")->setText("synthetic master"); dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        QTRY_VERIFY(service->status(id)->unlocked && !service->status(id)->busy); QTRY_VERIFY(!window.findChild<QDialog*>("filePassword"));
        QVERIFY(service->status(id)->passwordProtected == true); QCOMPARE(contents(path), before);
        QTest::qWait(50); QVERIFY(!window.findChild<QDialog*>("filePassword"));
        auto* tree = window.findChild<QTreeWidget*>("connectionTree"); QCOMPARE(tree->topLevelItem(0)->childCount(), 1);
    }
    void multipleProtectedFilesQueueIndependentUnlocks() {
        QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml"), c = dir.filePath("c.xml");
        QVERIFY(fixture(a, "127.0.0.1", "synthetic master A", true) && fixture(b, "localhost", "synthetic master B", true) && fixture(c, "127.0.0.3"));
        const auto originalA = contents(a), originalB = contents(b); MainWindow window(a, true); window.show();
        auto* service = window.findChild<DocumentService*>(); const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b), idC = window.openConnectionFile(c);
        QTRY_VERIFY(service->status(idC) && service->status(idC)->unlocked && !service->status(idC)->busy);
        QTRY_VERIFY(window.findChild<QDialog*>("filePassword")); QCOMPARE(window.findChildren<QDialog*>("filePassword").size(), 1);
        auto* dialog = window.findChild<QDialog*>("filePassword"); QVERIFY(dialog->windowTitle().contains("a.xml"));
        QPointer<QDialog> first(dialog); dialog->reject(); QTRY_VERIFY(first.isNull());
        QTRY_VERIFY(window.findChild<QDialog*>("filePassword")); dialog = window.findChild<QDialog*>("filePassword"); QVERIFY(dialog->windowTitle().contains("b.xml"));
        dialog->findChild<QLineEdit*>("masterEdit")->setText("synthetic master B"); dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        QTRY_VERIFY(service->status(idB)->unlocked && !service->status(idB)->busy); QTRY_VERIFY(!window.findChild<QDialog*>("filePassword"));
        QVERIFY(!service->status(idA)->unlocked && service->status(idC)->unlocked); QCOMPARE(contents(a), originalA); QCOMPARE(contents(b), originalB);
        QVERIFY(window.selectConnectionFile(idA)); window.findChild<QAction*>("actionUnlockPasswords")->trigger(); QTRY_VERIFY(window.findChild<QDialog*>("filePassword"));
        QVERIFY(window.selectConnectionFile(idB)); window.closeConnectionFile(); QTRY_VERIFY(!window.connectionFiles().contains(idB));
        QVERIFY(window.findChild<QDialog*>("filePassword")->windowTitle().contains("a.xml"));
        window.findChild<QDialog*>("filePassword")->reject(); QTRY_VERIFY(!window.findChild<QDialog*>("filePassword"));
    }
    void wrongPasswordRetriesBoundedAndCancelDoesNotWrite() {
        QTemporaryDir dir; const auto path = dir.filePath("protected.xml"); QVERIFY(fixture(path, "127.0.0.1", "synthetic master")); const auto before = contents(path);
        MainWindow window(path, true); window.show(); auto* service = window.findChild<DocumentService*>(); const auto id = window.connectionFiles().first();
        for (int attempt = 1; attempt <= 3; ++attempt) {
            QTRY_VERIFY(window.findChild<QDialog*>("filePassword")); auto* dialog = window.findChild<QDialog*>("filePassword");
            QVERIFY(dialog->findChild<QLabel*>("hintLabel")->text().contains(QString("Attempt %1 of 3").arg(attempt)));
            if (attempt > 1) QVERIFY(!dialog->findChild<QLabel*>("errorLabel")->text().isEmpty());
            QPointer<QDialog> previous(dialog); dialog->findChild<QLineEdit*>("masterEdit")->setText("incorrect synthetic master");
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click(); QTRY_VERIFY(previous.isNull());
            QTRY_VERIFY(!service->status(id)->busy); QCOMPARE(contents(path), before); QVERIFY(!service->status(id)->unlocked);
        }
        QTRY_VERIFY(!window.findChild<QDialog*>("filePassword")); QVERIFY(!window.lastError().isEmpty()); QCOMPARE(window.sessionCount(), size_t(0));
        QVERIFY(window.selectConnectionFile(id)); window.findChild<QAction*>("actionUnlockPasswords")->trigger(); QTRY_VERIFY(window.findChild<QDialog*>("filePassword"));
        window.findChild<QDialog*>("filePassword")->reject(); QTRY_VERIFY(!window.findChild<QDialog*>("filePassword"));
        QVERIFY(!service->status(id)->unlocked); QCOMPARE(contents(path), before); QVERIFY(!window.findChild<QDialog*>("credentialPrompt"));
        window.findChild<QAction*>("actionUnlockPasswords")->trigger(); QTRY_VERIFY(window.findChild<QDialog*>("filePassword"));
        window.closeConnectionFile(); QTRY_VERIFY(window.connectionFiles().isEmpty()); QTRY_VERIFY(!window.findChild<QDialog*>("filePassword")); QCOMPARE(contents(path), before);
    }
    void rootPasswordProtectAndFullEncryptionAreIndependentV71() {
        QTemporaryDir dir; const auto path = dir.filePath("a.xml"), other = dir.filePath("b.xml"); QVERIFY(fixture(path, "127.0.0.1") && fixture(other, "localhost"));
        const auto oldDefault = contents(path), originalOther = contents(other); const Secret custom(QByteArray("synthetic master")), initial(QByteArray("mR3m"));
        MainWindow window(path, true); window.show(); auto* service = window.findChild<DocumentService*>(); const auto id = window.connectionFiles().first(), idOther = window.openConnectionFile(other);
        QTRY_VERIFY(service->status(idOther) && !service->status(idOther)->busy && !service->status(id)->busy); QVERIFY(window.selectConnectionFile(id));
        setGridChoice(window, "protection", true); QTRY_VERIFY(window.findChild<QDialog*>("filePassword"));
        auto* dialog = window.findChild<QDialog*>("filePassword"); dialog->findChild<QLineEdit*>("masterEdit")->setText("synthetic master");
        dialog->findChild<QLineEdit*>("masterConfirmEdit")->setText("synthetic master"); dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        QTRY_VERIFY(!service->status(id)->busy && !window.findChild<ProfileEditor*>()->dirty()); QVERIFY(service->status(id)->passwordProtected == true && !service->status(id)->fullEncryption);
        QVERIFY(contents(path).contains("127.0.0.1")); QVERIFY(!ConnectionXml::read(contents(path), &initial)); QVERIFY(ConnectionXml::read(oldDefault, &initial));
        setGridChoice(window, "full_encryption", true); QTRY_VERIFY(window.findChild<QMessageBox*>("confirmProtection")); window.findChild<QMessageBox*>("confirmProtection")->button(QMessageBox::Yes)->click();
        QTRY_VERIFY(!service->status(id)->busy && !window.findChild<ProfileEditor*>()->dirty()); QVERIFY(service->status(id)->passwordProtected == true && service->status(id)->fullEncryption);
        QVERIFY(!window.findChild<QDialog*>("filePassword")); QVERIFY(!contents(path).contains("127.0.0.1")); const auto oldCustom = contents(path);
        setGridChoice(window, "protection", false); QTRY_VERIFY(window.findChild<QMessageBox*>("confirmRemoveMaster")); window.findChild<QMessageBox*>("confirmRemoveMaster")->button(QMessageBox::Yes)->click();
        QTRY_VERIFY(!service->status(id)->busy && !window.findChild<ProfileEditor*>()->dirty()); QVERIFY(service->status(id)->passwordProtected == false && service->status(id)->fullEncryption);
        QVERIFY(ConnectionXml::read(contents(path), &initial)); QVERIFY(!ConnectionXml::read(contents(path), &custom)); QVERIFY(ConnectionXml::read(oldCustom, &custom));
        setGridChoice(window, "full_encryption", false); QTRY_VERIFY(window.findChild<QMessageBox*>("confirmProtection")); window.findChild<QMessageBox*>("confirmProtection")->button(QMessageBox::Yes)->click();
        QTRY_VERIFY(!service->status(id)->busy && !window.findChild<ProfileEditor*>()->dirty()); QVERIFY(service->status(id)->passwordProtected == false && !service->status(id)->fullEncryption);
        QVERIFY(contents(path).contains("127.0.0.1")); QCOMPARE(contents(other), originalOther);
    }
    void cancelRemovingMasterPreservesFile() {
        QTemporaryDir dir; const auto path = dir.filePath("a.xml"); QVERIFY(fixture(path, "127.0.0.1", "synthetic master", true)); const auto before = contents(path);
        MainWindow window(path, true); window.show(); const auto id = window.connectionFiles().first(); auto* service = window.findChild<DocumentService*>();
        QTRY_VERIFY(service->status(id) && !service->status(id)->busy);
        window.unlockConnectionFile(id, Secret(QByteArray("synthetic master"))); QTRY_VERIFY(service->status(id)->unlocked && !service->status(id)->busy); QVERIFY(window.selectConnectionFile(id));
        setGridChoice(window, "protection", false); QTRY_VERIFY(window.findChild<QMessageBox*>("confirmRemoveMaster"));
        window.findChild<QMessageBox*>("confirmRemoveMaster")->button(QMessageBox::Cancel)->click(); QTRY_VERIFY(!window.findChild<ProfileEditor*>()->dirty());
        QCOMPARE(contents(path), before); QVERIFY(service->status(id)->passwordProtected == true && service->status(id)->fullEncryption);
        QVERIFY(window.findChild<ProfileEditor*>()->requestedProtection());
    }
    void productionDragCommitsOnlyOwningSubtree() {
        QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
        QVERIFY(fixture(a, "127.0.0.1") && fixture(b, "localhost")); const auto sourceB = contents(b);
        Barrier barrier; MainWindow window(a, true); Unblock unblock{barrier}; window.show();
        auto* service = window.findChild<DocumentService*>(); auto* tree = window.findChild<ConnectionTree*>("connectionTree"); QVERIFY(service && tree);
        const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
        QTRY_VERIFY(service->status(idA) && !service->status(idA)->busy && service->status(idB) && !service->status(idB)->busy);
        QVERIFY(window.selectConnectionFile(idA)); window.addFolder();
        QTRY_VERIFY(!window.selectedId().isEmpty() && window.selectedId() != Profile && !service->status(idA)->busy);
        const auto folder = window.selectedId(); const auto originalA = contents(a);
        const QPersistentModelIndex rootB(tree->model()->index(1, 0));
        const QPersistentModelIndex profileB(tree->model()->index(0, 0, rootB));
        QVERIFY(window.selectConnectionProfile({idA, Profile}));
        auto mime = std::unique_ptr<QMimeData>(tree->model()->mimeData({tree->currentIndex()})); QVERIFY(mime);
        QVERIFY(!drop(tree, mime.get(), tree->topLevelItem(1))); QCOMPARE(contents(a), originalA); QCOMPARE(contents(b), sourceB);
        bool ready = false; service->setCommitGate(idA, barrier.gate(), [&](DocumentReply reply) { ready = reply.result.ok; }); QTRY_VERIFY(ready);
        QVERIFY(drop(tree, mime.get(), item(tree, {idA, folder})));
        QTRY_COMPARE(barrier.entered.available(), 1);
        QCOMPARE(item(tree, {idA, Profile})->parent(), tree->topLevelItem(0)); QCOMPARE(contents(a), originalA);
        QVERIFY(window.selectConnectionFile(idB)); barrier.release.release();
        QTRY_VERIFY(!service->status(idA)->busy && item(tree, {idA, Profile})->parent() == item(tree, {idA, folder}));
        QCOMPARE(window.selectedDocument(), idB); QCOMPARE(contents(b), sourceB); QVERIFY(rootB.isValid() && profileB.isValid());
        auto xml = ConnectionXml::read(contents(a)); QVERIFY(xml && xml->unlock(Secret(QByteArray("mR3m"))));
        const auto projection = xml->projection(); QVERIFY(projection);
        for (const auto& value : QJsonDocument::fromJson(*projection).object()["profiles"].toArray())
            if (value.toObject()["id"] == Profile) QCOMPARE(value.toObject()["parent"].toString(), folder);
        QVERIFY(!drop(tree, mime.get(), tree->topLevelItem(0)));
        ready = false; service->setCommitGate(idA, {}, [&](DocumentReply reply) { ready = reply.result.ok; }); QTRY_VERIFY(ready);
    }
    void lockDuringConfigSaveReconcilesOnlyOwningCommit() {
        for (bool after : {false, true}) {
            QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
            QVERIFY(fixture(a, "127.0.0.1") && fixture(b, "localhost")); const auto sourceA = contents(a), sourceB = contents(b);
            Barrier barrier; barrier.after = after; MainWindow window(a, true); Unblock unblock{barrier}; window.show();
            auto* service = window.findChild<DocumentService*>(); QVERIFY(service);
            const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
            QTRY_VERIFY(service->status(idA) && !service->status(idA)->busy && service->status(idB) && !service->status(idB)->busy);
            bool ready = false; service->setCommitGate(idA, barrier.gate(), [&](DocumentReply reply) { ready = reply.result.ok; }); QTRY_VERIFY(ready);
            QVERIFY(window.selectConnectionProfile({idA, Profile})); setGridText(window, "name", "Owning committed edit");
            QTRY_COMPARE(barrier.entered.available(), 1);
            window.findChild<QAction*>("actionLockPasswords")->trigger();
            QVERIFY(!service->status(idA)->unlocked && service->status(idB)->unlocked); barrier.release.release();
            QTRY_VERIFY(!service->status(idA)->busy);
            if (after) {
                QTRY_VERIFY(!window.findChild<ProfileEditor*>()->dirty()); QVERIFY(contents(a).contains("Owning committed edit"));
                auto* tree = window.findChild<ConnectionTree*>("connectionTree"); QCOMPARE(item(tree, {idA, Profile})->text(0), QString("Owning committed edit"));
                QVERIFY(window.selectConnectionProfile({idB, Profile})); QCOMPARE(window.selectedDocument(), idB);
            } else {
                QTRY_VERIFY(!window.lastError().isEmpty()); QVERIFY(window.findChild<ProfileEditor*>()->dirty()); QCOMPARE(contents(a), sourceA);
                QVERIFY(window.selectConnectionProfile({idB, Profile})); QTRY_VERIFY(window.findChild<QMessageBox*>("saveRecovery"));
                QCOMPARE(window.selectedDocument(), idA); window.findChild<QMessageBox*>("saveRecovery")->done(QMessageBox::Discard);
                QTRY_COMPARE(window.selectedDocument(), idB);
            }
            QCOMPARE(contents(b), sourceB); QVERIFY(service->status(idB)->unlocked);
            ready = false; service->setCommitGate(idA, {}, [&](DocumentReply reply) { ready = reply.result.ok; }); QTRY_VERIFY(ready);
        }
    }
    void newSaveAsDuplicateAndEmptyWorkspace() {
        QTemporaryDir dir; const auto source = dir.filePath("new.xml"), copy = dir.filePath("copy.xml");
        MainWindow window(source, true); window.show();
        QTRY_COMPARE(window.connectionFiles().size(), 1); const auto id = window.connectionFiles().first();
        auto* service = window.findChild<DocumentService*>(); QVERIFY(service);
        QTRY_VERIFY(service->status(id) && !service->status(id)->busy);
        QVERIFY(!QFile::exists(source)); window.saveConnectionFile(); QTRY_VERIFY(QFile::exists(source));
        QTRY_VERIFY(!service->status(id)->busy); const auto original = contents(source);
        window.saveConnectionFileAs(copy); QTRY_VERIFY(QFile::exists(copy)); QTRY_VERIFY(!service->status(id)->busy);
        QCOMPARE(service->status(id)->path, copy); QCOMPARE(contents(source), original);
        window.openConnectionFile(copy); QTRY_COMPARE(window.connectionFiles().size(), 1);
        const auto other = window.openConnectionFile(source); QVERIFY(other != id); QTRY_COMPARE(window.connectionFiles().size(), 2);
        QTRY_VERIFY(service->status(other) && !service->status(other)->busy);
        window.closeConnectionFile(); QTRY_COMPARE(window.connectionFiles().size(), 1);
        QVERIFY(window.selectConnectionFile(id)); window.closeConnectionFile(); QTRY_COMPARE(window.connectionFiles().size(), 0);
        window.quickConnect("localhost:3389", true); QCOMPARE(window.sessionCount(), size_t(1));
        QVERIFY(window.findChild<QAction*>("actionOpenFile")->isEnabled()); QVERIFY(window.findChild<QAction*>("actionNewFile")->isEnabled());
        window.closeTab(0); QCOMPARE(window.sessionCount(), size_t(0));
    }
};
QTEST_MAIN(MultipleFilesTest)
#include "multiple_files.moc"
