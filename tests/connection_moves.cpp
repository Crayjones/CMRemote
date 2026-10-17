#include "MainWindow.h"
#include "ConnectionTree.h"
#include "GridDriver.h"
#include "SessionPane.h"
#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeData>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidgetItemIterator>

static QByteArray contents(const QString& path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
static QTreeWidgetItem* item(ConnectionTree* tree, const QString& id) {
    for (QTreeWidgetItemIterator it(tree); *it; ++it) if (treeProfileId(*it) == id) return *it;
    return nullptr;
}
static QJsonObject stored(const QString& path, const QString& id) {
    for (const auto& value : storedXml(path).value("profiles").toArray()) if (value.toObject().value("id") == id) return value.toObject();
    return {};
}
static std::unique_ptr<QMimeData> dragData(ConnectionTree* tree, const QString& id) {
    auto* source = item(tree, id); if (!source) return {};
    // Use the same model/virtual mimeData path as QDrag, not a move signal or profile API.
    tree->setCurrentItem(source);
    return std::unique_ptr<QMimeData>(tree->model()->mimeData({tree->currentIndex()}));
}
static bool drop(ConnectionTree* tree, QMimeData* mime, const QString& parent) {
    auto* target = item(tree, parent); if (!target || !mime) return false;
    tree->scrollToItem(target); const auto point = tree->visualItemRect(target).center();
    QDragEnterEvent enter(QPointF(point), Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(tree->viewport(), &enter);
    QDragMoveEvent move(QPointF(point), Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(tree->viewport(), &move);
    QDropEvent event(point, Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(tree->viewport(), &event); return event.isAccepted();
}
static bool dropOnItem(ConnectionTree* tree, QMimeData* mime, QTreeWidgetItem* target) {
    if (!target || !mime) return false;
    tree->scrollToItem(target); const auto point = tree->visualItemRect(target).center();
    QDragEnterEvent enter(QPointF(point), Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(tree->viewport(), &enter);
    QDragMoveEvent move(QPointF(point), Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(tree->viewport(), &move);
    QDropEvent event(point, Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(tree->viewport(), &event); return event.isAccepted();
}
// Drops at a vertical fraction of the target row: <0.25 inserts before,
// >0.75 after (folders), 0.5 drops into a folder or after a connection.
static bool dropAt(ConnectionTree* tree, QMimeData* mime, const QString& target, double fraction) {
    auto* row = item(tree, target); if (!row || !mime) return false;
    tree->scrollToItem(row); const auto rect = tree->visualItemRect(row);
    const QPoint point(rect.center().x(), rect.top() + int(rect.height() * fraction));
    QDragEnterEvent enter(QPointF(point), Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(tree->viewport(), &enter);
    QDragMoveEvent move(QPointF(point), Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(tree->viewport(), &move);
    QDropEvent event(point, Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(tree->viewport(), &event); return event.isAccepted();
}
static QStringList storedOrder(const QString& path, const QString& parent) {
    QStringList result;
    for (const auto& value : storedXml(path).value("profiles").toArray())
        if (value.toObject().value("parent").toString() == parent) result << value.toObject().value("id").toString();
    return result;
}
static QStringList treeOrder(QTreeWidgetItem* parent) {
    QStringList result; for (int i = 0; i < parent->childCount(); ++i) result << treeProfileId(parent->child(i)); return result;
}
static std::unique_ptr<QMimeData> dragItem(ConnectionTree* tree, QTreeWidgetItem* source) {
    tree->setCurrentItem(source); return std::unique_ptr<QMimeData>(tree->model()->mimeData({tree->currentIndex()}));
}
static QString add(MainWindow& window, bool folder, const QString& name) {
    if (folder) window.addFolder(); else window.addConnection();
    if (!waitGridSaved(window)) return {}; const auto id = window.selectedId(); setGridText(window, "name", name); if (!waitGridSaved(window)) return {}; return id;
}
class ConnectionMoves : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qApp->setQuitOnLastWindowClosed(false); }
    void crossDocumentDragIsRejected() {
        ConnectionTree tree; tree.resize(500, 500);
        const auto idA = DocumentId::create(), idB = DocumentId::create();
        const QHash<QString, QJsonObject> profiles{{"folder", {{"kind", "folder"}}}, {"host", {{"kind", "connection"}, {"parent", "folder"}}}};
        QHash<DocumentId, DocumentTreeState> documents{{idA, {profiles, 11, true}}, {idB, {profiles, 21, true}}};
        tree.setCommittedDocuments(documents); tree.setMovesEnabled(true);
        auto* rootA = new QTreeWidgetItem(&tree, {"Connections — A"}, ConnectionTree::DocumentItem);
        auto* rootB = new QTreeWidgetItem(&tree, {"Connections — B"}, ConnectionTree::DocumentItem);
        rootA->setData(0, Qt::UserRole, QVariant::fromValue(DocumentRootReference{idA})); rootB->setData(0, Qt::UserRole, QVariant::fromValue(DocumentRootReference{idB}));
        auto* folderA = new QTreeWidgetItem(rootA, {"Folder"}, ConnectionTree::ProfileItem);
        auto* folderB = new QTreeWidgetItem(rootB, {"Folder"}, ConnectionTree::ProfileItem);
        auto* hostA = new QTreeWidgetItem(folderA, {"Host"}, ConnectionTree::ProfileItem);
        auto* hostB = new QTreeWidgetItem(folderB, {"Host"}, ConnectionTree::ProfileItem);
        for (const auto& entry : std::initializer_list<std::pair<QTreeWidgetItem*, ProfileReference>>{{folderA, {idA, "folder"}}, {folderB, {idB, "folder"}}, {hostA, {idA, "host"}}, {hostB, {idB, "host"}}})
            entry.first->setData(0, Qt::UserRole, QVariant::fromValue(entry.second));
        tree.expandAll(); tree.show(); QApplication::processEvents();
        QVERIFY(tree.validMove(ProfileReference{idA, "host"}, DocumentRootReference{idA}));
        QVERIFY(!tree.validMove(ProfileReference{idA, "host"}, DocumentRootReference{idB}));
        auto mimeA = dragItem(&tree, hostA), mimeB = dragItem(&tree, hostB); QVERIFY(mimeA && mimeB);
        QSignalSpy requested(&tree, &ConnectionTree::scopedMoveRequested);
        QVERIFY(!dropOnItem(&tree, mimeA.get(), rootB)); QVERIFY(!dropOnItem(&tree, mimeA.get(), folderB)); QCOMPARE(requested.count(), 0);
        QVERIFY(dropOnItem(&tree, mimeA.get(), rootA)); QCOMPARE(requested.count(), 1);
        QCOMPARE(requested[0][0].value<ProfileReference>(), (ProfileReference{idA, "host"}));
        QCOMPARE(std::get<DocumentRootReference>(requested[0][1].value<ConnectionDestination>()).document, idA);
        QCOMPARE(hostA->parent(), folderA); QCOMPARE(hostB->parent(), folderB);
        QMimeData external; external.setData("application/x-remotemanager-profile", mimeA->data("application/x-remotemanager-profile"));
        QVERIFY(!dropOnItem(&tree, &external, rootA));
        ConnectionTree other; other.resize(500, 500); other.setCommittedDocuments(documents); other.setMovesEnabled(true);
        auto* foreignRoot = new QTreeWidgetItem(&other, {"Connections"}, ConnectionTree::DocumentItem);
        foreignRoot->setData(0, Qt::UserRole, QVariant::fromValue(DocumentRootReference{idA})); other.show(); QApplication::processEvents();
        QVERIFY(!dropOnItem(&other, mimeA.get(), foreignRoot));
        ++documents[idA].generation; tree.setCommittedDocuments(documents);
        QVERIFY(!dropOnItem(&tree, mimeA.get(), rootA)); QVERIFY(dropOnItem(&tree, mimeB.get(), rootB)); QCOMPARE(requested.count(), 2);
        documents[idA].movesEnabled = false; ++documents[idA].generation; tree.setCommittedDocuments(documents); QVERIFY(!dragItem(&tree, hostA));
        mimeB = dragItem(&tree, hostB); QVERIFY(mimeB); QVERIFY(dropOnItem(&tree, mimeB.get(), rootB));
        documents.remove(idB); tree.setCommittedDocuments(documents); QVERIFY(!dropOnItem(&tree, mimeB.get(), rootB));
    }
    void dragConnectionIntoFolderPersists() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); QString id, folder;
        {
            MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
            id = add(window, false, "Host"); auto* tree = window.findChild<ConnectionTree*>("connectionTree"); QVERIFY(tree); tree->setCurrentItem(item(tree, {})); folder = add(window, true, "Folder");
            auto mime = dragData(tree, id); const auto original = contents(path); QVERIFY(drop(tree, mime.get(), folder));
            QCOMPARE(item(tree, id)->parent(), item(tree, {})); QCOMPARE(contents(path), original);
            QTRY_COMPARE(stored(path, id).value("parent").toString(), folder); QVERIFY(waitGridSaved(window)); QCOMPARE(item(tree, id)->parent(), item(tree, folder)); QCOMPARE(window.selectedId(), id);
        }
        MainWindow restart(path, true); QTRY_COMPARE(restart.documentState(), MainWindow::DocumentState::Open); auto* tree = restart.findChild<ConnectionTree*>("connectionTree"); QCOMPARE(item(tree, id)->parent(), item(tree, folder));
    }
    void dragFolderMovesSubtreeWithStableIds() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        auto* tree = window.findChild<ConnectionTree*>("connectionTree"); const auto folder = add(window, true, "Subtree"), child = add(window, false, "Child"); const auto before = stored(path, child);
        tree->setCurrentItem(item(tree, {})); const auto destination = add(window, true, "Destination"); auto mime = dragData(tree, folder); QVERIFY(drop(tree, mime.get(), destination));
        QTRY_COMPARE(stored(path, folder).value("parent").toString(), destination); QVERIFY(waitGridSaved(window)); QCOMPARE(item(tree, child)->parent(), item(tree, folder)); QCOMPARE(stored(path, child), before);
        QCOMPARE(item(tree, folder)->parent(), item(tree, destination));
    }
    void dropBetweenSiblingsReordersAndPersists() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); QString a, b, c, folder;
        {
            MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
            auto* tree = window.findChild<ConnectionTree*>("connectionTree");
            a = add(window, false, "A"); tree->setCurrentItem(item(tree, {})); b = add(window, false, "B");
            tree->setCurrentItem(item(tree, {})); c = add(window, false, "C"); tree->setCurrentItem(item(tree, {})); folder = add(window, true, "Folder");
            QCOMPARE(storedOrder(path, {}), QStringList({a, b, c, folder}));
            auto mime = dragData(tree, c); QVERIFY(dropAt(tree, mime.get(), a, 0.1));
            QTRY_COMPARE(storedOrder(path, {}), QStringList({c, a, b, folder})); QVERIFY(waitGridSaved(window)); QCOMPARE(treeOrder(item(tree, {})), QStringList({c, a, b, folder}));
            mime = dragData(tree, c); QVERIFY(dropAt(tree, mime.get(), b, 0.9));
            QTRY_COMPARE(storedOrder(path, {}), QStringList({a, b, c, folder})); QVERIFY(waitGridSaved(window));
            // No-op placements are rejected without a write.
            const auto original = contents(path); mime = dragData(tree, a); QVERIFY(!dropAt(tree, mime.get(), b, 0.1)); QVERIFY(!dropAt(tree, mime.get(), a, 0.9));
            QTest::qWait(50); QCOMPARE(contents(path), original);
            // Folder middle drops inside; folder edges insert beside it.
            mime = dragData(tree, b); QVERIFY(dropAt(tree, mime.get(), folder, 0.5)); QTRY_COMPARE(storedOrder(path, folder), QStringList({b})); QVERIFY(waitGridSaved(window));
            item(tree, folder)->setExpanded(true);
            mime = dragData(tree, a); QVERIFY(dropAt(tree, mime.get(), b, 0.1)); QTRY_COMPARE(storedOrder(path, folder), QStringList({a, b})); QVERIFY(waitGridSaved(window));
            mime = dragData(tree, c); QVERIFY(!dropAt(tree, mime.get(), folder, 0.1));
            item(tree, folder)->setExpanded(false);
            QVERIFY(dropAt(tree, mime.get(), folder, 0.9)); QTRY_COMPARE(storedOrder(path, {}), QStringList({folder, c})); QVERIFY(waitGridSaved(window));
        }
        MainWindow restart(path, true); QTRY_COMPARE(restart.documentState(), MainWindow::DocumentState::Open);
        auto* tree = restart.findChild<ConnectionTree*>("connectionTree");
        QCOMPARE(treeOrder(item(tree, {})), QStringList({folder, c})); QCOMPARE(treeOrder(item(tree, folder)), QStringList({a, b}));
    }
    void folderConfigHidesHostname() {
        QTemporaryDir dir; MainWindow window(dir.filePath("p"), true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        add(window, true, "Folder"); QVERIFY(gridRow(window, "host")->isHidden()); QVERIFY(!gridRow(window, "username")->isHidden());
        add(window, false, "Host"); QVERIFY(!gridRow(window, "host")->isHidden());
    }
    void folderConnectOpensChildrenWithFolderAccount() {
        QTemporaryDir dir; MainWindow window(dir.filePath("p"), true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        auto* tree = window.findChild<ConnectionTree*>("connectionTree");
        const auto folder = add(window, true, "Folder"); setGridText(window, "username", "folder-user"); QVERIFY(waitGridSaved(window));
        const auto first = add(window, false, "First"); setGridText(window, "host", "first.invalid"); QVERIFY(waitGridSaved(window));
        setGridText(window, "username", ""); QVERIFY(waitGridSaved(window));
        tree->setCurrentItem(item(tree, folder)); const auto second = add(window, false, "Second"); setGridText(window, "host", "second.invalid"); QVERIFY(waitGridSaved(window));
        setGridText(window, "username", "own-user"); QVERIFY(waitGridSaved(window));
        tree->setCurrentItem(item(tree, folder)); add(window, false, "No host");
        // Double-clicking a folder only expands/collapses it.
        tree->setCurrentItem(item(tree, folder)); QVERIFY(tree->expandsOnDoubleClick());
        emit tree->itemDoubleClicked(item(tree, folder), 0); QTest::qWait(50); QCOMPARE(window.sessionCount(), size_t(0));
        QVERIFY(!window.findChild<QDialog*>("CredentialsDialog"));
#ifdef RM_WITH_RDP
        // A connection still starts on double-click (here: its credentials prompt).
        tree->setCurrentItem(item(tree, second)); emit tree->itemDoubleClicked(item(tree, second), 0);
        QTRY_VERIFY(window.findChild<QDialog*>("CredentialsDialog")); window.findChild<QDialog*>("CredentialsDialog")->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QCOMPARE(window.sessionCount(), size_t(0));
#endif
        tree->setCurrentItem(item(tree, folder));
#ifdef RM_WITH_RDP
        QVERIFY(window.findChild<QAction*>("actionTreeConnect")->isEnabled());
#endif
        window.connectSelected(true);
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); QTRY_COMPARE(tabs->count(), 2); QCOMPARE(window.sessionCount(), size_t(2));
        auto* paneA = qobject_cast<SessionPane*>(tabs->widget(0)); auto* paneB = qobject_cast<SessionPane*>(tabs->widget(1)); QVERIFY(paneA && paneB);
        QCOMPARE(paneA->profile(), first); QCOMPARE(paneA->target().username, QString("folder-user"));
        QCOMPARE(paneB->profile(), second); QCOMPARE(paneB->target().username, QString("own-user"));
        // Connecting again focuses the existing tabs instead of duplicating them.
        tree->setCurrentItem(item(tree, folder)); window.connectSelected(true); QTest::qWait(50); QCOMPARE(tabs->count(), 2);
    }
    void dropOnConnectionsRootClearsParent() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        const auto folder = add(window, true, "Folder"), id = add(window, false, "Host"); auto* tree = window.findChild<ConnectionTree*>("connectionTree"); auto mime = dragData(tree, id); QVERIFY(drop(tree, mime.get(), {}));
        QTRY_VERIFY(stored(path, id).value("parent").isNull()); QVERIFY(waitGridSaved(window)); QCOMPARE(item(tree, id)->parent(), item(tree, {}));
        const auto original = contents(path); mime = dragData(tree, id); QVERIFY(!drop(tree, mime.get(), {})); QTest::qWait(50); QCOMPARE(contents(path), original); QVERIFY(item(tree, folder));
    }
    void invalidAndForeignDropsDoNotMutate() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        const auto folder = add(window, true, "Folder"), nested = add(window, true, "Nested"), host = add(window, false, "Host"); auto* tree = window.findChild<ConnectionTree*>("connectionTree"); const auto original = contents(path);
        auto mime = dragData(tree, folder); QVERIFY(!drop(tree, mime.get(), folder)); QVERIFY(!drop(tree, mime.get(), nested)); QVERIFY(!drop(tree, mime.get(), host));
        auto root = dragData(tree, {}); QVERIFY(!root || !drop(tree, root.get(), folder));
        QMimeData external; external.setData("application/x-remotemanager-profile", mime->data("application/x-remotemanager-profile")); QVERIFY(!drop(tree, &external, {}));
        MainWindow other(dir.filePath("other"), true); other.show(); QTRY_COMPARE(other.documentState(), MainWindow::DocumentState::Open); auto* foreign = other.findChild<ConnectionTree*>("connectionTree"); QVERIFY(!drop(foreign, mime.get(), {}));
        QCOMPARE(contents(path), original); QCOMPARE(item(tree, nested)->parent(), item(tree, folder)); QCOMPARE(item(tree, host)->parent(), item(tree, nested));
        QVERIFY(setDocumentFault(window, AtomicFile::Fault::BeforeReplace));
        tree->setCurrentItem(item(tree, host)); setGridText(window, "name", "Pending"); QTRY_VERIFY(!window.lastError().isEmpty()); mime = dragData(tree, host); QVERIFY(!drop(tree, mime.get(), {})); QCOMPARE(contents(path), original);
    }
    void protectedMoveRequiresUnlock() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); QString id, folder;
        {
            MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
            id = add(window, false, "Private host"); auto* tree = window.findChild<ConnectionTree*>("connectionTree"); tree->setCurrentItem(item(tree, {})); folder = add(window, true, "Private folder");
        }
        const Secret initial(QByteArray("mR3m")), master(QByteArray("synthetic move master"));
        DocumentStore store(path); QVERIFY(store.loadXml(&initial).result.ok);
        QVERIFY(store.updateXml(initial, [&](ConnectionXml& xml) { return xml.setFullEncryption(initial, true) && xml.changeMaster(initial, master); }, &master).ok);
        const auto original = contents(path);
        MainWindow window(path, true); window.show(); auto* tree = window.findChild<ConnectionTree*>("connectionTree");
        QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Locked); QVERIFY(!item(tree, id)); QVERIFY(!tree->dragEnabled());
        QTRY_VERIFY(window.findChild<QDialog*>("filePassword")); auto* unlock = window.findChild<QDialog*>("filePassword");
        unlock->findChild<QLineEdit*>("masterEdit")->setText("synthetic move master"); unlock->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open); QTRY_VERIFY(!window.findChild<QDialog*>("filePassword")); QCOMPARE(contents(path), original);
        auto* service = window.findChild<DocumentService*>(); service->lock(window.selectedDocument()); QTRY_VERIFY(!tree->dragEnabled());
        QVERIFY(!dragData(tree, id)); QCOMPARE(contents(path), original); QCOMPARE(item(tree, id)->parent(), item(tree, {}));
        window.retryDocumentUnlock(); QTRY_VERIFY(window.findChild<QDialog*>("filePassword")); window.findChild<QDialog*>("filePassword")->reject();
        QTRY_VERIFY(!window.findChild<QDialog*>("filePassword")); QVERIFY(!tree->dragEnabled()); QCOMPARE(contents(path), original);
        window.retryDocumentUnlock(); QTRY_VERIFY(window.findChild<QDialog*>("filePassword")); unlock = window.findChild<QDialog*>("filePassword");
        unlock->findChild<QLineEdit*>("masterEdit")->setText("synthetic move master"); unlock->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        QTRY_VERIFY(tree->dragEnabled()); auto mime = dragData(tree, id); QVERIFY(drop(tree, mime.get(), folder));
        QTRY_COMPARE(item(tree, id)->parent(), item(tree, folder)); QVERIFY(waitGridSaved(window));
        QCOMPARE(storedXml(path, master)["profiles"].toArray().size(), 2);
        auto reopened = ConnectionXml::read(contents(path), &master); QVERIFY(reopened && reopened->fullEncryption());
        QVERIFY(!ConnectionXml::read(contents(path), &initial));
        MainWindow restart(path, true); restart.show(); QTRY_COMPARE(restart.documentState(), MainWindow::DocumentState::Locked);
        QTRY_VERIFY(restart.findChild<QDialog*>("filePassword")); unlock = restart.findChild<QDialog*>("filePassword");
        unlock->findChild<QLineEdit*>("masterEdit")->setText("synthetic move master"); unlock->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        QTRY_COMPARE(restart.documentState(), MainWindow::DocumentState::Open);
        QCOMPARE(item(restart.findChild<ConnectionTree*>("connectionTree"), id)->parent(), item(restart.findChild<ConnectionTree*>("connectionTree"), folder));
    }
    void movesRecomputeInheritanceWithoutRetargetingSessions() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        auto* tree = window.findChild<ConnectionTree*>("connectionTree"); const auto source = add(window, true, "Old defaults");
        setGridText(window, "username", "account"); QVERIFY(waitGridSaved(window));
        setGridText(window, "domain", "OLD"); QVERIFY(waitGridSaved(window)); setGridChoice(window, "resolution", "1280x720"); QVERIFY(waitGridSaved(window));
        const auto id = add(window, false, "Host"); setGridText(window, "host", "localhost"); QVERIFY(waitGridSaved(window));
        const auto local = stored(path, id).value("overrides"); window.connectSelected(true);
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); auto* pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane); const auto session = pane->id(); const auto capture = pane->target();
        tree->setCurrentItem(item(tree, {})); const auto destination = add(window, true, "New defaults");
        setGridText(window, "domain", "NEW"); QVERIFY(waitGridSaved(window));
        setGridChoice(window, "resolution", "800x600"); QVERIFY(waitGridSaved(window)); setGridChoice(window, "color_depth", 16); QVERIFY(waitGridSaved(window));
        auto mime = dragData(tree, id); QVERIFY(drop(tree, mime.get(), destination)); QTRY_COMPARE(stored(path, id).value("parent").toString(), destination); QVERIFY(waitGridSaved(window));
        QCOMPARE(stored(path, id).value("overrides"), local); QCOMPARE(gridValue(window, "domain").toString(), QString("NEW")); QCOMPARE(gridValue(window, "host").toString(), QString("localhost"));
        QCOMPARE(pane->id(), session); QCOMPARE(pane->profile(), id); QVERIFY(pane->target() == capture); QCOMPARE(window.sessionCount(), size_t(1)); QVERIFY(item(tree, source));
        window.reconnectCurrent(); pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane); QVERIFY(pane->id() != session); QCOMPARE(pane->profile(), id);
        QCOMPARE(pane->target().domain, QString("NEW")); QCOMPARE(pane->target().host, QString("localhost")); QCOMPARE(pane->target().options.resolution, QString("800x600")); QCOMPARE(pane->target().options.colorDepth, quint8(16));
    }
    void dragAfterCellEditWaitsForSave() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        const auto id = add(window, false, "Host"); auto* tree = window.findChild<ConnectionTree*>("connectionTree"); tree->setCurrentItem(item(tree, {})); const auto folder = add(window, true, "Folder");
        auto mime = dragData(tree, id); const auto before = contents(path); XmlCommitBarrier barrier; QVERIFY(setDocumentGate(window, barrier.gate()));
        auto* edit = qobject_cast<QLineEdit*>(gridEditor(window, "name")); QVERIFY(edit); edit->selectAll(); QTest::keyClicks(edit, "Edited before move");
        QVERIFY(drop(tree, mime.get(), folder)); QTRY_VERIFY(barrier.entered()); QCOMPARE(contents(path), before); QCOMPARE(item(tree, id)->parent(), item(tree, {})); QVERIFY(!tree->dragEnabled());
        barrier.release(); QTRY_VERIFY(barrier.entered(2)); QCOMPARE(stored(path, id).value("name").toString(), QString("Edited before move")); QVERIFY(stored(path, id).value("parent").isNull()); QCOMPARE(item(tree, id)->parent(), item(tree, {}));
        barrier.release(); QTRY_COMPARE(stored(path, id).value("parent").toString(), folder); QVERIFY(waitGridSaved(window)); QCOMPARE(item(tree, id)->parent(), item(tree, folder));
    }
    void sortAndSearchPreserveMovedIdentity() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        auto* tree = window.findChild<ConnectionTree*>("connectionTree"); const auto id = add(window, false, "Zulu match"); tree->setCurrentItem(item(tree, {})); const auto folder = add(window, true, "Alpha match");
        tree->setCurrentItem(item(tree, {})); const auto hidden = add(window, false, "Hidden host");
        window.findChild<QAction*>("actionSort")->menu()->actions().at(1)->trigger(); auto* search = window.findChild<QLineEdit*>("searchEdit"); search->setText("match"); QVERIFY(item(tree, hidden)->isHidden());
        item(tree, folder)->setExpanded(true); item(tree, {})->setExpanded(true); auto mime = dragData(tree, id); QVERIFY(drop(tree, mime.get(), folder)); QTRY_COMPARE(stored(path, id).value("parent").toString(), folder); QVERIFY(waitGridSaved(window));
        QCOMPARE(window.selectedId(), id); QCOMPARE(search->text(), QString("match")); QVERIFY(item(tree, hidden)->isHidden()); QVERIFY(!item(tree, id)->isHidden()); QVERIFY(!item(tree, folder)->isHidden()); QVERIFY(!item(tree, {})->isHidden()); QVERIFY(item(tree, folder)->isExpanded());
        search->clear(); QCOMPARE(treeProfileId(item(tree, {})->child(0)), hidden); QCOMPARE(item(tree, id)->parent(), item(tree, folder));
    }
    void mouseGestureStartsIdentityDragWithoutReparenting() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        const auto id = add(window, false, "Host"); auto* tree = window.findChild<ConnectionTree*>("connectionTree"); const auto original = contents(path);
        QSignalSpy started(tree, &ConnectionTree::dragStarting); const auto start = tree->visualItemRect(item(tree, id)).center();
        QTest::mousePress(tree->viewport(), Qt::LeftButton, Qt::NoModifier, start);
        const auto end = start + QPoint(QApplication::startDragDistance() + 10, 0);
        QTimer::singleShot(50, tree, [] { QDrag::cancel(); });
        QMouseEvent motion(QEvent::MouseMove, end, tree->viewport()->mapToGlobal(end), Qt::NoButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(tree->viewport(), &motion);
        QTest::mouseRelease(tree->viewport(), Qt::LeftButton, Qt::NoModifier, end); QCOMPARE(started.count(), 1); QCOMPARE(contents(path), original); QCOMPARE(item(tree, id)->parent(), item(tree, {}));
    }
    void contradictoryInheritedAccountRejectsMove() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        const auto id = add(window, false, "Qualified account"); setGridText(window, "username", "QUALIFIED\\account"); QVERIFY(waitGridSaved(window));
        auto* tree = window.findChild<ConnectionTree*>("connectionTree"); tree->setCurrentItem(item(tree, {})); const auto folder = add(window, true, "Domain default"); setGridText(window, "domain", "SEPARATE"); QVERIFY(waitGridSaved(window));
        auto mime = dragData(tree, id); const auto original = contents(path); QVERIFY(drop(tree, mime.get(), folder)); QTRY_VERIFY(!window.lastError().isEmpty()); QCOMPARE(contents(path), original); QCOMPARE(item(tree, id)->parent(), item(tree, {}));
        tree->setCurrentItem(item(tree, folder)); setGridText(window, "domain", ""); QVERIFY(waitGridSaved(window)); mime = dragData(tree, id); QVERIFY(drop(tree, mime.get(), folder)); QTRY_COMPARE(stored(path, id).value("parent").toString(), folder); QVERIFY(waitGridSaved(window));
    }
    void externalChangeRejectsMoveWithoutOverwrite() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        const auto id = add(window, false, "Host"); auto* tree = window.findChild<ConnectionTree*>("connectionTree"); tree->setCurrentItem(item(tree, {})); const auto folder = add(window, true, "Folder"); auto mime = dragData(tree, id);
        const auto external = contents(path) + "\n"; QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(external), external.size()); file.close();
        QVERIFY(drop(tree, mime.get(), folder)); QTRY_VERIFY(!window.lastError().isEmpty()); QCOMPARE(contents(path), external); QCOMPARE(item(tree, id)->parent(), item(tree, {}));
    }
    void durabilityWarningRetainsCommittedMoveAndGatesFurtherDrops() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        const auto id = add(window, false, "Host"); auto* tree = window.findChild<ConnectionTree*>("connectionTree"); tree->setCurrentItem(item(tree, {})); const auto folder = add(window, true, "Folder"); auto mime = dragData(tree, id);
        QVERIFY(setDocumentFault(window, AtomicFile::Fault::DirectorySync));
        QVERIFY(drop(tree, mime.get(), folder)); QTRY_VERIFY(window.findChild<QMessageBox*>("durabilityWarning")); QCOMPARE(stored(path, id).value("parent").toString(), folder); QCOMPARE(item(tree, id)->parent(), item(tree, folder)); QVERIFY(!tree->dragEnabled());
        const auto committed = contents(path); QVERIFY(!drop(tree, mime.get(), {})); QCOMPARE(contents(path), committed);
        window.findChild<QMessageBox*>("durabilityWarning")->button(QMessageBox::Ok)->click(); QTRY_VERIFY(tree->dragEnabled()); QCOMPARE(contents(path), committed);
    }
    void movedDomainInvalidatesSavedPasswordAuthorization() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); QString id, destination;
        {
            MainWindow seed(path, true); seed.show(); QTRY_COMPARE(seed.documentState(), MainWindow::DocumentState::Open); auto* tree = seed.findChild<ConnectionTree*>("connectionTree");
            add(seed, true, "Old defaults"); setGridText(seed, "username", "account"); QVERIFY(waitGridSaved(seed)); setGridText(seed, "domain", "OLD"); QVERIFY(waitGridSaved(seed)); id = add(seed, false, "Host");
            setGridText(seed, "host", "127.0.0.1"); QVERIFY(waitGridSaved(seed));
            tree->setCurrentItem(item(tree, {})); destination = add(seed, true, "New defaults"); setGridText(seed, "username", "account"); QVERIFY(waitGridSaved(seed)); setGridText(seed, "domain", "NEW"); QVERIFY(waitGridSaved(seed));
        }
        const Secret master(QByteArray("mR3m")); SessionTarget target{"127.0.0.1", 3389, "account", "OLD"};
        DocumentStore store(path); QVERIFY(store.loadXml(&master).result.ok);
        QVERIFY(store.updateXml(master, [&](ConnectionXml& xml) {
            return xml.setPassword(master, id, Secret(QByteArray("synthetic move password"))) && xml.reviewPasswords(master, {{id, target}});
        }).ok);
        QVERIFY(store.xml()->password(master, id, target));
        MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        auto* tree = window.findChild<ConnectionTree*>("connectionTree"); tree->setCurrentItem(item(tree, id)); window.connectSelected(true);
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); QTRY_COMPARE(tabs->count(), 1); auto* pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane);
        const auto session = pane->id(); QCOMPARE(pane->target().domain, QString("OLD"));
        auto mime = dragData(tree, id); QVERIFY(drop(tree, mime.get(), destination)); QTRY_COMPARE(stored(path, id).value("parent").toString(), destination); QVERIFY(waitGridSaved(window));
        QCOMPARE(pane->id(), session); QCOMPARE(pane->target().domain, QString("OLD"));
        auto reopened = ConnectionXml::read(contents(path), &master); QVERIFY(reopened); auto changed = target; changed.domain = "NEW";
        QVERIFY(!reopened->password(master, id, target)); QVERIFY(!reopened->password(master, id, changed));
        window.reconnectCurrent(); pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane); QCOMPARE(pane->target().domain, QString("NEW"));
    }
    void failedMovePreservesTreeModelAndFile() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        const auto id = add(window, false, "Host"); auto* tree = window.findChild<ConnectionTree*>("connectionTree"); tree->setCurrentItem(item(tree, {})); const auto folder = add(window, true, "Folder"); auto mime = dragData(tree, id); const auto original = contents(path);
        QVERIFY(setDocumentFault(window, AtomicFile::Fault::BeforeReplace));
        QVERIFY(drop(tree, mime.get(), folder)); QTRY_VERIFY(!window.lastError().isEmpty()); QCOMPARE(contents(path), original); QCOMPARE(item(tree, id)->parent(), item(tree, {})); QCOMPARE(window.selectedId(), id);
    }
};
QTEST_MAIN(ConnectionMoves)
#include "connection_moves.moc"
