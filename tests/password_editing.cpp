#include "MainWindow.h"
#include "ProfileEditor.h"
#include "PropertyDelegate.h"
#include "GridDriver.h"
#include "SessionPane.h"
#include "FilePasswordDialogs.h"
#include <QLabel>
#include <QTabWidget>
#if defined(Q_OS_UNIX) && defined(RM_WITH_RDP)
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QVBoxLayout>
#include <QJsonArray>
#include <QSemaphore>
#include <QMenu>
#include <QContextMenuEvent>
#include <QCheckBox>

namespace {
QTreeWidgetItem* row(MainWindow& window, const QString& key) {
    auto* tree = window.findChild<QTreeWidget*>("configTree");
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->data(0, PropertyDelegate::KeyRole).toString() == key) return *it;
    return nullptr;
}
QByteArray contents(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
bool settled(MainWindow& window, DocumentId id) {
    auto* files = window.findChild<DocumentService*>(); auto* editor = window.findChild<ProfileEditor*>();
    return QTest::qWaitFor([&] { return files->status(id) && !files->status(id)->busy && !editor->dirty(); });
}
void assertSecretFree(QWidget& window, const QByteArray& secret) {
    const auto* editor = window.findChild<ProfileEditor*>(); QVERIFY(editor);
    QVERIFY(!QJsonDocument(editor->draft()).toJson().contains(secret));
    auto* tree = window.findChild<QTreeWidget*>("configTree"); QVERIFY(tree);
    for (QTreeWidgetItemIterator it(tree); *it; ++it) for (int column = 0; column < 2; ++column) {
        for (int role = Qt::DisplayRole; role <= PropertyDelegate::CategoryRole; ++role)
            QVERIFY(!(*it)->data(column, role).toString().toUtf8().contains(secret));
    }
}
const QString Folder = "11111111-1111-4111-8111-111111111111", Consumer = "22222222-2222-4222-8222-222222222222";
bool fixture(const QString& path, const QString& host, quint16 port = 3389) {
    auto xml = ConnectionXml::create(); Secret initial(QByteArray("mR3m")), master(QByteArray("synthetic file master"));
    const QJsonObject folder{{"id", Folder}, {"kind", "folder"}, {"name", "Folder"}, {"parent", QJsonValue()},
        {"overrides", QJsonObject{{"host", host}, {"username", "synthetic-user"}, {"protocol", "rdp"}, {"port", int(port)}}}};
    const QJsonObject child{{"id", Consumer}, {"kind", "connection"}, {"name", "Child"}, {"parent", Folder}, {"overrides", QJsonObject{}}};
    const auto projection = QJsonDocument(QJsonObject{{"schema_version", 2}, {"profiles", QJsonArray{folder, child}}}).toJson(QJsonDocument::Compact);
    if (!xml || !xml->patch(initial, projection) || !xml->setPasswordInheritance(initial, Consumer, true) || !xml->changeMaster(initial, master)) return false;
    const auto bytes = xml->serialize(master); QFile file(path);
    return bytes && file.open(QIODevice::WriteOnly) && file.write(*bytes) == bytes->size();
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
struct Unblock { Barrier& barrier; ~Unblock() { barrier.release.release(); } };
#if defined(Q_OS_UNIX) && defined(RM_WITH_RDP)
// Owned loopback listener: never replies with an RDP challenge, so these prompt
// tests can start/abort a worker without authenticating to any existing service.
struct PromptListener {
    int descriptor = -1; quint16 port = 0;
    PromptListener() {
        descriptor = ::socket(AF_INET, SOCK_STREAM, 0); if (descriptor < 0) return;
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof(address);
        if (::bind(descriptor, reinterpret_cast<sockaddr*>(&address), size) == 0 && ::listen(descriptor, 4) == 0 &&
            ::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &size) == 0) port = ntohs(address.sin_port);
    }
    ~PromptListener() { if (descriptor >= 0) ::close(descriptor); }
};
#endif
void acceptReview(MainWindow& window) {
    // Entering a password binds it directly; no reuse review prompt appears.
    QVERIFY(!window.findChild<QMessageBox*>("passwordReuseReview"));
}
void enterMaster(QDialog* dialog) {
    auto* first = dialog->findChild<QLineEdit*>("masterEdit"); auto* second = dialog->findChild<QLineEdit*>("masterConfirmEdit");
    QVERIFY(first && second); first->setFocus(); QTest::keyClicks(first, "synthetic file master");
    second->setFocus(); QTest::keyClicks(second, "synthetic file master");
    QTest::mouseClick(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
}
}
class PasswordEditingTest : public QObject {
    Q_OBJECT
private slots:
    void passwordEnterFocusEscapeAndClear() {
        QWidget window; window.resize(650, 750); auto* layout = new QVBoxLayout(&window);
        auto* tree = new QTreeWidget(&window); tree->setObjectName("configTree"); layout->addWidget(tree);
        auto* focus = new QLineEdit(&window); focus->setPlaceholderText("Focus target"); layout->addWidget(focus);
        auto* editor = new ProfileEditor(tree);
        editor->load({{"id", "11111111-1111-4111-8111-111111111111"}, {"kind", "connection"}, {"name", "Synthetic"}, {"overrides", QJsonObject{}}}, {});
        int calls = 0; PasswordEdit::Kind last = PasswordEdit::Kind::Clear;
        editor->setPasswordHandler([&](PasswordEdit edit) { ++calls; last = edit.kind; if (edit.kind == PasswordEdit::Kind::Set) QVERIFY(edit.value.size() != 0); });
        editor->setPasswordState({{}, true, false, true, true}, true);
        auto* delegate = qobject_cast<PropertyDelegate*>(tree->itemDelegate()); QSignalSpy variants(delegate, &PropertyDelegate::valueCommitted);
        window.show();
        auto* line = qobject_cast<QLineEdit*>(gridEditor(window, "password")); QVERIFY(line);
        QCOMPARE(line->echoMode(), QLineEdit::Password); QVERIFY(line->text().isEmpty());
        QTest::keyClick(line, Qt::Key_Return); QTRY_VERIFY(!delegate->activeEditor()); QCOMPARE(calls, 0);
        line = qobject_cast<QLineEdit*>(gridEditor(window, "password", false)); QVERIFY(line);
        QTest::keyClicks(line, "synthetic-first-password"); QTest::keyClick(line, Qt::Key_Escape);
        QTRY_VERIFY(!delegate->activeEditor()); QCOMPARE(calls, 0); QVERIFY(!editor->dirty());
        line = qobject_cast<QLineEdit*>(gridEditor(window, "password")); QVERIFY(line);
        QTest::keyClicks(line, "synthetic-first-password"); QTest::keyClick(line, Qt::Key_Return);
        QTRY_VERIFY(!delegate->activeEditor()); QCOMPARE(calls, 1); QVERIFY(editor->dirty());
        assertSecretFree(window, "synthetic-first-password"); editor->finishPasswordEdit();
        line = qobject_cast<QLineEdit*>(gridEditor(window, "password", false)); QVERIFY(line); QVERIFY(line->text().isEmpty());
        QTest::keyClicks(line, "synthetic-keypad-password"); QTest::keyClick(line, Qt::Key_Enter, Qt::KeypadModifier);
        QTRY_VERIFY(!delegate->activeEditor()); QCOMPARE(calls, 2); assertSecretFree(window, "synthetic-keypad-password"); editor->finishPasswordEdit();
        line = qobject_cast<QLineEdit*>(gridEditor(window, "password")); QVERIFY(line);
        QTest::keyClicks(line, "synthetic-focus-password"); QTest::mouseClick(focus, Qt::LeftButton); QTRY_VERIFY(focus->hasFocus());
        QTRY_VERIFY(!delegate->activeEditor()); QCOMPARE(calls, 3); assertSecretFree(window, "synthetic-focus-password"); editor->finishPasswordEdit();
        editor->clearPassword(); QCOMPARE(calls, 4); QVERIFY(last == PasswordEdit::Kind::Clear); QCOMPARE(variants.count(), 0);
        editor->requirePasswordReentry(); QVERIFY(editor->dirty()); editor->finishPasswordEdit(); QVERIFY(!editor->dirty());
    }
    void previewCannotStoreSecrets() {
        QWidget window; auto* tree = new QTreeWidget(&window); tree->setObjectName("configTree"); auto* editor = new ProfileEditor(tree);
        editor->load({{"id", "11111111-1111-4111-8111-111111111111"}, {"kind", "connection"}, {"name", "Preview"}}, {});
        editor->setPasswordState({{}, true, false, true, true}, true);
        QVERIFY(!gridRow(window, "password")->flags().testFlag(Qt::ItemIsEditable));
        QVERIFY(!editor->canEditPassword()); editor->editPassword(); editor->clearPassword(); QVERIFY(!editor->dirty());
        auto* delegate = qobject_cast<PropertyDelegate*>(tree->itemDelegate());
        QVERIFY(!delegate->activeEditor());
    }
    void folderPasswordInheritanceIsFileScoped() {
        QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
        QVERIFY(fixture(a, "127.0.0.1") && fixture(b, "127.0.0.2")); const auto beforeB = contents(b);
        MainWindow window(a, true); window.show(); auto* files = window.findChild<DocumentService*>();
        const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
        QTRY_VERIFY(files->status(idA) && !files->status(idA)->busy && files->status(idB) && !files->status(idB)->busy);
        window.unlockConnectionFile(idA, Secret(QByteArray("synthetic file master"))); QTRY_VERIFY(files->status(idA)->unlocked && !files->status(idA)->busy);
        window.unlockConnectionFile(idB, Secret(QByteArray("synthetic file master"))); QTRY_VERIFY(files->status(idB)->unlocked && !files->status(idB)->busy);
        QVERIFY(window.selectConnectionProfile({idA, Folder}));
        setGridText(window, "password", "synthetic-folder-password"); acceptReview(window); QVERIFY(settled(window, idA));
        QVERIFY(files->status(idA)->passwords.value(Consumer).inherited);
        QVERIFY(files->status(idA)->passwords.value(Consumer).authorized); QCOMPARE(contents(b), beforeB);
        SessionTarget target; target.host = "127.0.0.1"; target.username = "synthetic-user";
        std::optional<DocumentReply> reply;
        files->lookup({idA, Consumer}, target, [&](DocumentReply value) { reply.emplace(std::move(value)); });
        QTRY_VERIFY(reply.has_value()); QVERIFY(reply->result.ok && reply->password);
        QVERIFY(reply->password->takeBytes() == QByteArray("synthetic-folder-password")); reply.reset();
        target.host = "127.0.0.2";
        files->lookup({idB, Consumer}, target, [&](DocumentReply value) { reply.emplace(std::move(value)); });
        QTRY_VERIFY(reply.has_value()); QVERIFY(reply->result.ok && !reply->password); reply.reset();
        QVERIFY(window.selectConnectionProfile({idA, Folder})); setGridText(window, "username", "changed-user"); QVERIFY(settled(window, idA));
        QVERIFY(files->status(idA)->passwords.value(Consumer).present); QVERIFY(!files->status(idA)->passwords.value(Consumer).authorized);
        target.host = "127.0.0.1"; target.username = "changed-user";
        files->lookup({idA, Consumer}, target, [&](DocumentReply value) { reply.emplace(std::move(value)); });
        QTRY_VERIFY(reply.has_value()); QVERIFY(reply->result.ok && !reply->password); QCOMPARE(contents(b), beforeB);
        assertSecretFree(window, "synthetic-folder-password");
    }
    void nativeImportReviewDoesNotSaveOnUnlock() {
        QTemporaryDir dir; const auto a = dir.filePath("native.xml"), b = dir.filePath("other.xml");
        QVERIFY(fixture(a, "127.0.0.1") && fixture(b, "127.0.0.2"));
        Secret master(QByteArray("synthetic file master")); auto xml = ConnectionXml::read(contents(a), &master); QVERIFY(xml);
        QVERIFY(xml->setPassword(master, Folder, Secret(QByteArray("synthetic-native-password"))));
        const auto native = xml->serialize(master); QVERIFY(native); QVERIFY(!native->contains("RmFormatVersion"));
        { QFile file(a); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(*native), native->size()); }
        const auto before = contents(a), other = contents(b);
        MainWindow window(a, true); window.show(); auto* files = window.findChild<DocumentService*>();
        const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
        QTRY_VERIFY(files->status(idA) && !files->status(idA)->busy && files->status(idB) && !files->status(idB)->busy);
        window.unlockConnectionFile(idA, master.clone());
        QTRY_VERIFY(window.findChild<QMessageBox*>("nativePasswordReview"));
        auto* review = window.findChild<QMessageBox*>("nativePasswordReview");
        QVERIFY(review->text().contains("127.0.0.1:3389") && review->text().contains("synthetic-user"));
        QVERIFY(!review->text().contains("synthetic-native-password")); QCOMPARE(contents(a), before);
        QVERIFY(!files->status(idA)->passwords.value(Consumer).authorized);
        QTest::mouseClick(review->button(QMessageBox::Cancel), Qt::LeftButton);
        QTRY_VERIFY(!window.findChild<QMessageBox*>("nativePasswordReview")); QCOMPARE(contents(a), before);
        QVERIFY(files->status(idA)->reviewRequired);
        QVERIFY(window.selectConnectionFile(idA)); auto* unlock = window.findChild<QAction*>("actionUnlockPasswords");
        QVERIFY(unlock->isEnabled()); unlock->trigger();
        QTRY_VERIFY(window.findChild<QMessageBox*>("nativePasswordReview")); review = window.findChild<QMessageBox*>("nativePasswordReview");
        QVERIFY(window.selectConnectionFile(idB)); QCOMPARE(window.selectedDocument(), idB);
        QTest::mouseClick(review->button(QMessageBox::Yes), Qt::LeftButton);
        QTRY_VERIFY(!files->status(idA)->busy && files->status(idA)->passwords.value(Consumer).authorized);
        QVERIFY(!files->status(idA)->reviewRequired); QCOMPARE(window.selectedDocument(), idB);
        QCOMPARE(contents(a), before); QCOMPARE(contents(b), other);
        SessionTarget target; target.host = "127.0.0.1"; target.username = "synthetic-user";
        std::optional<DocumentReply> reply;
        files->lookup({idA, Consumer}, target, [&](DocumentReply value) { reply.emplace(std::move(value)); });
        QTRY_VERIFY(reply.has_value()); QVERIFY(reply->result.ok && reply->password);
        QVERIFY(reply->password->takeBytes() == QByteArray("synthetic-native-password")); reply.reset();
        QVERIFY(window.selectConnectionFile(idA)); window.saveConnectionFile();
        QTRY_VERIFY(!files->status(idA)->busy && contents(a) != before);
        QVERIFY(contents(a).contains("RmCredentialBindings")); QCOMPARE(contents(b), other);
        auto saved = ConnectionXml::read(contents(a), &master); QVERIFY(saved && !saved->credentialReviewRequired());
        QVERIFY(saved->password(master, Consumer, target)); assertSecretFree(window, "synthetic-native-password");
    }
    void nativePasswordReviewRejectsUnreviewedTargets() {
        QTemporaryDir dir; const auto path = dir.filePath("native.xml"); QVERIFY(fixture(path, "127.0.0.1"));
        Secret master(QByteArray("synthetic file master")); auto xml = ConnectionXml::read(contents(path), &master); QVERIFY(xml);
        QVERIFY(xml->setPassword(master, Folder, Secret(QByteArray("synthetic-native-password"))));
        const auto native = xml->serialize(master); QVERIFY(native);
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(*native), native->size()); }
        DocumentService files; std::optional<DocumentReply> reply;
        const auto id = files.open(path, [&](DocumentReply value) { reply.emplace(std::move(value)); });
        QTRY_VERIFY(reply.has_value()); QVERIFY(reply->result.ok); reply.reset();
        files.unlock(id, master.clone(), [&](DocumentReply value) { reply.emplace(std::move(value)); });
        QTRY_VERIFY(reply.has_value()); QVERIFY(reply->result.ok); reply.reset();
        SessionTarget target; target.host = "127.0.0.1"; target.username = "synthetic-user";
        for (int invalid = 0; invalid < 5; ++invalid) {
            auto changed = target; auto consumer = Consumer;
            if (invalid == 0) changed.host = "127.0.0.2";
            if (invalid == 1) changed.username = "unreviewed-user";
            if (invalid == 2) changed.domain = "unreviewed-domain";
            if (invalid == 3) consumer = Folder;
            std::vector<PasswordAuthorization> reviewed{{consumer, changed}};
            if (invalid == 4) reviewed.push_back({consumer, changed});
            files.reviewPasswords(id, std::move(reviewed), [&](DocumentReply value) { reply.emplace(std::move(value)); });
            QTRY_VERIFY(reply.has_value()); QVERIFY(!reply->result.ok && !reply->committed); reply.reset();
            QVERIFY(files.status(id)->reviewRequired); QVERIFY(!files.status(id)->passwords.value(Consumer).authorized);
            QCOMPARE(contents(path), *native);
        }
        files.reviewPasswords(id, {{Consumer, target}}, [&](DocumentReply value) { reply.emplace(std::move(value)); });
        QTRY_VERIFY(reply.has_value()); QVERIFY(reply->result.ok && !reply->committed); reply.reset();
        QVERIFY(files.status(id)->passwords.value(Consumer).authorized); QCOMPARE(contents(path), *native);
        { QFile file(path); QVERIFY(file.open(QIODevice::Append)); QCOMPARE(file.write("\n"), qint64(1)); }
        files.lookup({id, Consumer}, target, [&](DocumentReply value) { reply.emplace(std::move(value)); });
        QTRY_VERIFY(reply.has_value()); QVERIFY(!reply->result.ok && !reply->password); reply.reset();
        files.reviewPasswords(id, {{Consumer, target}}, [&](DocumentReply value) { reply.emplace(std::move(value)); });
        QTRY_VERIFY(reply.has_value()); QVERIFY(!reply->result.ok && !reply->committed); reply.reset();
        QCOMPARE(contents(path), *native + '\n');
        files.lock(id);
        files.lookup({id, Consumer}, target, [&](DocumentReply value) { reply.emplace(std::move(value)); });
        QTRY_VERIFY(reply.has_value()); QVERIFY(!reply->result.ok && !reply->password);
    }
    void nativeImportReviewLockLeavesFileUnchanged() {
        QTemporaryDir dir; const auto path = dir.filePath("native.xml"); QVERIFY(fixture(path, "127.0.0.1"));
        Secret master(QByteArray("synthetic file master")); auto xml = ConnectionXml::read(contents(path), &master); QVERIFY(xml);
        QVERIFY(xml->setPassword(master, Folder, Secret(QByteArray("synthetic-native-password"))));
        const auto native = xml->serialize(master); QVERIFY(native);
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(*native), native->size()); }
        MainWindow window(path, true); window.show(); auto* files = window.findChild<DocumentService*>(); const auto id = window.connectionFiles().first();
        QTRY_VERIFY(files->status(id) && !files->status(id)->busy); window.unlockConnectionFile(id, master.clone());
        QTRY_VERIFY(window.findChild<QMessageBox*>("nativePasswordReview")); files->lock(id);
        QTRY_VERIFY(!window.findChild<QMessageBox*>("nativePasswordReview"));
        QVERIFY(!files->status(id)->unlocked && files->status(id)->reviewRequired); QCOMPARE(contents(path), *native);
        window.unlockConnectionFile(id, master.clone()); QTRY_VERIFY(window.findChild<QMessageBox*>("nativePasswordReview"));
        auto* review = window.findChild<QMessageBox*>("nativePasswordReview");
        QTest::mouseClick(review->button(QMessageBox::Cancel), Qt::LeftButton); QCOMPARE(contents(path), *native);
    }
    void missingIdsRequireExplicitRepairSave() {
        QTemporaryDir dir; const auto path = dir.filePath("missing-ids.xml"); QVERIFY(fixture(path, "127.0.0.1"));
        auto native = contents(path); native.replace("Id=\"" + Folder.toUtf8() + '"', ""); native.replace("Id=\"" + Consumer.toUtf8() + '"', "");
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(native), native.size()); }
        MainWindow window(path, true); window.show(); auto* files = window.findChild<DocumentService*>(); const auto id = window.connectionFiles().first();
        QTRY_VERIFY(files->status(id) && !files->status(id)->busy && files->status(id)->repairRequired);
        auto* tree = window.findChild<QTreeWidget*>("connectionTree");
        auto childReference = [&] {
            for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->text(0) == "Child") return (*it)->data(0, Qt::UserRole).value<ProfileReference>();
            return ProfileReference{};
        };
        const auto temporary = childReference(); QVERIFY(temporary.valid());
        window.unlockConnectionFile(id, Secret(QByteArray("synthetic file master"))); QTRY_VERIFY(files->status(id)->unlocked && !files->status(id)->busy);
        QCOMPARE(childReference(), temporary); QCOMPARE(contents(path), native);
        QVERIFY(!window.findChild<QAction*>("actionSaveFile")->isEnabled());
        auto* action = window.findChild<QAction*>("actionRepairIds"); QVERIFY(action && action->isEnabled()); action->trigger();
        QTRY_VERIFY(window.findChild<QMessageBox*>("confirmIdRepair")); auto* question = window.findChild<QMessageBox*>("confirmIdRepair");
        QTest::mouseClick(question->button(QMessageBox::Cancel), Qt::LeftButton); QCOMPARE(contents(path), native); QCOMPARE(childReference(), temporary);
        bool done = false; files->setFault(id, AtomicFile::Fault::BeforeReplace, [&](DocumentReply value) { done = value.result.ok; }); QTRY_VERIFY(done);
        action->trigger(); QTRY_VERIFY(window.findChild<QMessageBox*>("confirmIdRepair")); question = window.findChild<QMessageBox*>("confirmIdRepair");
        QTest::mouseClick(question->button(QMessageBox::Yes), Qt::LeftButton); QTRY_VERIFY(!files->status(id)->busy && !window.lastError().isEmpty());
        QCOMPARE(contents(path), native); QCOMPARE(childReference(), temporary); QVERIFY(files->status(id)->repairRequired);
        done = false; files->setFault(id, AtomicFile::Fault::None, [&](DocumentReply value) { done = value.result.ok; }); QTRY_VERIFY(done);
        action->trigger(); QTRY_VERIFY(window.findChild<QMessageBox*>("confirmIdRepair")); question = window.findChild<QMessageBox*>("confirmIdRepair");
        QTest::mouseClick(question->button(QMessageBox::Yes), Qt::LeftButton); QTRY_VERIFY(!files->status(id)->busy && !files->status(id)->repairRequired);
        QCOMPARE(childReference(), temporary); QVERIFY(contents(path) != native);
        Secret master(QByteArray("synthetic file master")); auto repaired = ConnectionXml::read(contents(path), &master); QVERIFY(repaired);
        QVERIFY(!repaired->requiresIdRepair() && repaired->profileIds().contains(temporary.profile));
        QVERIFY(window.selectConnectionProfile(temporary)); QVERIFY(gridRow(window, "password")->flags().testFlag(Qt::ItemIsEditable));
    }
    void defaultKeyImportsNeedReviewNotConversion_data() {
        QTest::addColumn<bool>("missingIds"); QTest::addColumn<QString>("protocol");
        QTest::newRow("RDP native IDs") << false << QString("rdp");
        QTest::newRow("RDP ID repair under default key") << true << QString("rdp");
        QTest::newRow("SSH native IDs") << false << QString("ssh");
        QTest::newRow("SSH ID repair under default key") << true << QString("ssh");
    }
    void defaultKeyImportsNeedReviewNotConversion() {
        QFETCH(bool, missingIds); QFETCH(QString, protocol);
        QTemporaryDir dir; const auto path = dir.filePath("default.xml"); const Secret initial(QByteArray("mR3m"));
        const QByteArray cipher("8LmIO3+MWBY0zTmfjfOEdCGxhTAwnlohb1veTGNZFt6lAYvY2UOzWyjVzkx6V93smpbP0ZOuexN15u7rvwJEjawC");
        auto native = QByteArray("<Connections xmlns=\"http://mremoteng.org\" Name=\"Connections\" Export=\"false\" ConfVersion=\"2.8\" EncryptionEngine=\"AES\" BlockCipherMode=\"GCM\" KdfIterations=\"1000\" FullFileEncryption=\"false\" Protected=\"") + cipher +
            "\"><Node xmlns=\"\" Type=\"Connection\" Id=\"" + Consumer.toUtf8() +
            "\" Name=\"Default-key\" Hostname=\"127.0.0.1\" Protocol=\"RDP\" Port=\"3389\" Username=\"synthetic-user\" Colors=\"Colors32Bit\" Resolution=\"FitToWindow\" UseCredSsp=\"true\" RDPAuthenticationLevel=\"WarnOnFailedAuth\" RedirectSound=\"DoNotPlay\" Password=\"" + cipher + "\"/></Connections>";
        if (protocol == "ssh") native.replace("Protocol=\"RDP\"", "Protocol=\"SSH2\" Domain=\"passive /@ domain\"").replace("Port=\"3389\"", "Port=\"22\"");
        if (missingIds) native.replace("Id=\"" + Consumer.toUtf8() + '"', "");
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(native), native.size()); }
        MainWindow window(path, true); window.show(); auto* files = window.findChild<DocumentService*>(); const auto id = window.connectionFiles().first();
        QTRY_VERIFY(files->status(id) && !files->status(id)->busy); QVERIFY(files->status(id)->unlocked && files->status(id)->passwordProtected == false);
        QVERIFY(!window.findChild<QDialog*>("filePassword")); QCOMPARE(contents(path), native); QCOMPARE(files->status(id)->repairRequired, missingIds);
        QString consumer;
        for (QTreeWidgetItemIterator it(window.findChild<QTreeWidget*>("connectionTree")); *it; ++it)
            if ((*it)->text(0) == "Default-key") consumer = (*it)->data(0, Qt::UserRole).value<ProfileReference>().profile;
        QVERIFY(!consumer.isEmpty());
        if (missingIds) {
            auto* action = window.findChild<QAction*>("actionRepairIds"); QVERIFY(action && action->isEnabled()); action->trigger();
            QTRY_VERIFY(window.findChild<QMessageBox*>("confirmIdRepair")); window.findChild<QMessageBox*>("confirmIdRepair")->button(QMessageBox::Cancel)->click();
            QTRY_VERIFY(!window.findChild<QMessageBox*>("confirmIdRepair")); QCOMPARE(contents(path), native);
            bool done = false; files->setFault(id, AtomicFile::Fault::BeforeReplace, [&](DocumentReply reply) { done = reply.result.ok; }); QTRY_VERIFY(done);
            action->trigger(); QTRY_VERIFY(window.findChild<QMessageBox*>("confirmIdRepair")); window.findChild<QMessageBox*>("confirmIdRepair")->button(QMessageBox::Yes)->click();
            QTRY_VERIFY(!files->status(id)->busy && !window.lastError().isEmpty()); QCOMPARE(contents(path), native); QVERIFY(files->status(id)->repairRequired);
            done = false; files->setFault(id, AtomicFile::Fault::None, [&](DocumentReply reply) { done = reply.result.ok; }); QTRY_VERIFY(done);
            action->trigger(); QTRY_VERIFY(window.findChild<QMessageBox*>("confirmIdRepair")); window.findChild<QMessageBox*>("confirmIdRepair")->button(QMessageBox::Yes)->click();
        } else window.findChild<QAction*>("actionUnlockPasswords")->trigger();
        QTRY_VERIFY(window.findChild<QMessageBox*>("nativePasswordReview")); QVERIFY(!window.findChild<QDialog*>("filePassword"));
        auto* review = window.findChild<QMessageBox*>("nativePasswordReview"); review->button(QMessageBox::Cancel)->click();
        QTRY_VERIFY(!window.findChild<QMessageBox*>("nativePasswordReview")); QVERIFY(!files->status(id)->passwords.value(consumer).authorized);
        const auto beforeReview = contents(path); window.findChild<QAction*>("actionUnlockPasswords")->trigger();
        QTRY_VERIFY(window.findChild<QMessageBox*>("nativePasswordReview")); window.findChild<QMessageBox*>("nativePasswordReview")->button(QMessageBox::Yes)->click();
        QTRY_VERIFY(!files->status(id)->busy && files->status(id)->passwords.value(consumer).authorized); QCOMPARE(contents(path), beforeReview);
        QVERIFY(!files->status(id)->repairRequired); window.saveConnectionFile(); QTRY_VERIFY(!files->status(id)->busy && contents(path) != beforeReview);
        auto saved = ConnectionXml::read(contents(path), &initial); QVERIFY(saved && saved->profileIds().contains(consumer)); QVERIFY(saved->passwordProtected() == false);
        SessionTarget target; target.host = "127.0.0.1"; target.username = "synthetic-user"; target.protocol = protocol;
        if (protocol == "ssh") target.port = 22;
        std::optional<DocumentReply> reply;
        files->lookup({id, consumer}, target, [&](DocumentReply value) { reply.emplace(std::move(value)); }); QTRY_VERIFY(reply.has_value()); QVERIFY(reply->result.ok && reply->password);
        QVERIFY(reply->password->takeBytes() == QByteArray("ThisIsNotProtected")); assertSecretFree(window, "ThisIsNotProtected");
    }
    void inheritedPasswordMenuKeepsCapturedFileAndField() {
        QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
        Secret master(QByteArray("synthetic file master"));
        for (const auto& path : {a, b}) {
            QVERIFY(fixture(path, "127.0.0.1")); auto xml = ConnectionXml::read(contents(path), &master); QVERIFY(xml);
            QVERIFY(xml->setPasswordInheritance(master, Consumer, false)); const auto bytes = xml->serialize(master); QVERIFY(bytes);
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(*bytes), bytes->size());
        }
        const auto beforeB = contents(b);
        MainWindow window(a, true); window.show(); auto* files = window.findChild<DocumentService*>();
        const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
        QTRY_VERIFY(files->status(idA) && !files->status(idA)->busy && files->status(idB) && !files->status(idB)->busy);
        window.unlockConnectionFile(idA, master.clone()); QTRY_VERIFY(files->status(idA)->unlocked && !files->status(idA)->busy);
        window.unlockConnectionFile(idB, master.clone()); QTRY_VERIFY(files->status(idB)->unlocked && !files->status(idB)->busy);
        QVERIFY(window.selectConnectionProfile({idA, Consumer})); auto* tree = window.findChild<QTreeWidget*>("configTree");
        auto* password = gridRow(window, "password"); QVERIFY(password); tree->setCurrentItem(password, 1); tree->scrollToItem(password);
        const QPoint point(tree->columnViewportPosition(1) + tree->columnWidth(1) / 2, tree->visualItemRect(password).center().y());
        QContextMenuEvent event(QContextMenuEvent::Mouse, point, tree->viewport()->mapToGlobal(point)); QApplication::sendEvent(tree->viewport(), &event);
        QTRY_VERIFY(window.findChild<QMenu*>("configContextMenu")); auto* menu = window.findChild<QMenu*>("configContextMenu");
        QAction* inherit = nullptr; for (auto* action : menu->actions()) if (action->text() == "Use inherited value") inherit = action;
        QVERIFY(inherit && inherit->isEnabled());
        QVERIFY(window.selectConnectionProfile({idB, Consumer})); tree->setCurrentItem(gridRow(window, "password"), 1);
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(inherit).center());
        QTRY_VERIFY(files->status(idA)->passwords.value(Consumer).inherited);
        QVERIFY(settled(window, idA)); QCOMPARE(window.selectedDocument(), idA);
        QVERIFY(!files->status(idB)->passwords.value(Consumer).inherited); QCOMPARE(contents(b), beforeB);
    }
    void folderPasswordLookupServesFolderConnect() {
        QTemporaryDir dir; const auto path = dir.filePath("a.xml"); QVERIFY(fixture(path, "127.0.0.1"));
        MainWindow window(path, true); window.show(); auto* files = window.findChild<DocumentService*>(); const auto id = window.connectionFiles().first();
        QTRY_VERIFY(files->status(id) && !files->status(id)->busy);
        window.unlockConnectionFile(id, Secret(QByteArray("synthetic file master"))); QTRY_VERIFY(files->status(id)->unlocked && !files->status(id)->busy);
        auto lookup = [&](const QString& profile) {
            std::optional<DocumentReply> reply; files->lookupFolder({id, profile}, [&](DocumentReply value) { reply.emplace(std::move(value)); });
            return QTest::qWaitFor([&] { return reply.has_value(); }, 5000) ? std::move(reply) : std::nullopt;
        };
        auto reply = lookup(Folder); QVERIFY(reply && reply->result.ok && !reply->password);
        QVERIFY(window.selectConnectionProfile({id, Folder})); setGridText(window, "password", "synthetic-folder-password"); acceptReview(window); QVERIFY(settled(window, id));
        reply = lookup(Folder); QVERIFY(reply && reply->result.ok && reply->password);
        QCOMPARE(reply->password->takeBytes(), QByteArray("synthetic-folder-password"));
        reply = lookup(Consumer); QVERIFY(reply && reply->result.ok && !reply->password);
    }
    void sameIdSessionsReconnectToTheirOwningFile() {
        QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml"); QVERIFY(fixture(a, "127.0.0.1") && fixture(b, "127.0.0.2"));
        MainWindow window(a, true); window.show(); auto* files = window.findChild<DocumentService*>();
        const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
        QTRY_VERIFY(files->status(idA) && !files->status(idA)->busy && files->status(idB) && !files->status(idB)->busy);
        for (const auto id : {idA, idB}) {
            window.unlockConnectionFile(id, Secret(QByteArray("synthetic file master"))); QTRY_VERIFY(files->status(id)->unlocked && !files->status(id)->busy);
            QVERIFY(window.selectConnectionProfile({id, Folder}));
            setGridText(window, "password", id == idA ? "synthetic-password-A" : "synthetic-password-B"); acceptReview(window); QVERIFY(settled(window, id));
            QVERIFY(window.selectConnectionProfile({id, Consumer})); window.connectSelected(true);
        }
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); QVERIFY(tabs); QCOMPARE(tabs->count(), 2); QCOMPARE(window.sessionCount(), size_t(2));
        auto* paneA = qobject_cast<SessionPane*>(tabs->widget(0)); auto* paneB = qobject_cast<SessionPane*>(tabs->widget(1)); QVERIFY(paneA && paneB);
        QVERIFY(paneA->reference() == ProfileReference({idA, Consumer})); QVERIFY(paneB->reference() == ProfileReference({idB, Consumer}));
        QCOMPARE(paneA->target().host, QString("127.0.0.1")); QCOMPARE(paneB->target().host, QString("127.0.0.2"));
        const auto beforeB = contents(b); QVERIFY(window.selectConnectionProfile({idA, Consumer}));
        setGridText(window, "host", "127.0.0.3"); QVERIFY(settled(window, idA));
        QCOMPARE(paneA->target().host, QString("127.0.0.1")); QCOMPARE(paneB->target().host, QString("127.0.0.2"));
        QVERIFY(window.selectConnectionProfile({idA, Folder})); setGridText(window, "password", "synthetic-replacement-A"); acceptReview(window); QVERIFY(settled(window, idA));
        QVERIFY(window.selectConnectionFile(idB)); tabs->setCurrentIndex(0); const auto previousA = paneA->id();
        window.disconnectCurrent(); QCOMPARE(window.sessionCount(), size_t(2)); window.reconnectCurrent();
        paneA = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(paneA && paneA->id() != previousA);
        QVERIFY(paneA->reference() == ProfileReference({idA, Consumer})); QCOMPARE(paneA->target().host, QString("127.0.0.3"));
        QVERIFY(window.selectConnectionFile(idA)); tabs->setCurrentIndex(1); const auto previousB = paneB->id(); window.reconnectCurrent();
        paneB = qobject_cast<SessionPane*>(tabs->widget(1)); QVERIFY(paneB && paneB->id() != previousB);
        QVERIFY(paneB->reference() == ProfileReference({idB, Consumer})); QCOMPARE(paneB->target().host, QString("127.0.0.2"));
        for (auto* pane : {paneA, paneB}) {
            std::optional<DocumentReply> reply; files->lookup(pane->reference(), pane->target(), [&](DocumentReply value) { reply.emplace(std::move(value)); });
            QTRY_VERIFY(reply.has_value()); QVERIFY(reply->result.ok && reply->password);
            QVERIFY(reply->password->takeBytes() == (pane->document() == idA ? QByteArray("synthetic-replacement-A") : QByteArray("synthetic-password-B")));
        }
        QCOMPARE(contents(b), beforeB); window.closeTab(1); window.closeTab(0); QCOMPARE(window.sessionCount(), size_t(0));
    }
    void changedHostKeepsSavedPassword() {
        QTemporaryDir dir; const auto path = dir.filePath("host.xml"); QVERIFY(fixture(path, "127.0.0.1"));
        MainWindow window(path, true); window.show(); auto* files = window.findChild<DocumentService*>(); const auto id = window.connectionFiles().first();
        QTRY_VERIFY(files->status(id) && !files->status(id)->busy); window.unlockConnectionFile(id, Secret(QByteArray("synthetic file master")));
        QTRY_VERIFY(files->status(id)->unlocked && !files->status(id)->busy); QVERIFY(window.selectConnectionProfile({id, Folder}));
        setGridText(window, "password", "synthetic-host-password"); acceptReview(window); QVERIFY(settled(window, id));
        QVERIFY(files->status(id)->passwords.value(Consumer).authorized);
        QVERIFY(window.selectConnectionProfile({id, Consumer})); setGridText(window, "host", "127.0.0.9"); QVERIFY(settled(window, id));
        QVERIFY(files->status(id)->passwords.value(Consumer).authorized);
        SessionTarget moved; moved.host = "127.0.0.9"; moved.username = "synthetic-user"; auto original = moved; original.host = "127.0.0.1";
        std::optional<DocumentReply> reply; files->lookup({id, Consumer}, moved, [&](DocumentReply value) { reply.emplace(std::move(value)); });
        QTRY_VERIFY(reply.has_value()); QVERIFY(reply->result.ok && reply->password);
        QVERIFY(reply->password->takeBytes() == QByteArray("synthetic-host-password"));
        reply.reset(); files->lookup({id, Consumer}, original, [&](DocumentReply value) { reply.emplace(std::move(value)); });
        QTRY_VERIFY(reply.has_value()); QVERIFY(reply->result.ok && !reply->password);
        QVERIFY(!contents(path).contains("synthetic-host-password")); assertSecretFree(window, "synthetic-host-password");
    }
    void hostChangedOutsideAppDoesNotReusePassword() {
        QTemporaryDir dir; const auto path = dir.filePath("outside.xml"); QVERIFY(fixture(path, "127.0.0.1"));
        {
            MainWindow window(path, true); window.show(); auto* files = window.findChild<DocumentService*>(); const auto id = window.connectionFiles().first();
            QTRY_VERIFY(files->status(id) && !files->status(id)->busy); window.unlockConnectionFile(id, Secret(QByteArray("synthetic file master")));
            QTRY_VERIFY(files->status(id)->unlocked && !files->status(id)->busy); QVERIFY(window.selectConnectionProfile({id, Folder}));
            setGridText(window, "password", "synthetic-outside-password"); acceptReview(window); QVERIFY(settled(window, id));
            QVERIFY(files->status(id)->passwords.value(Consumer).authorized);
        }
        // Another program retargets the file while CMRemote is closed.
        auto bytes = contents(path); QVERIFY(bytes.contains("Hostname=\"127.0.0.1\""));
        bytes.replace("Hostname=\"127.0.0.1\"", "Hostname=\"127.0.0.66\"");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write(bytes), qint64(bytes.size())); file.close();
        MainWindow window(path, true); window.show(); auto* files = window.findChild<DocumentService*>(); const auto id = window.connectionFiles().first();
        QTRY_VERIFY(files->status(id) && !files->status(id)->busy); window.unlockConnectionFile(id, Secret(QByteArray("synthetic file master")));
        QTRY_VERIFY(files->status(id)->unlocked && !files->status(id)->busy);
        QVERIFY(!files->status(id)->passwords.value(Consumer).authorized);
        // An unrelated in-app edit afterwards must not trust the retargeted host.
        QVERIFY(window.selectConnectionProfile({id, Consumer})); setGridText(window, "description", "edited later"); QVERIFY(settled(window, id));
        QVERIFY(!files->status(id)->passwords.value(Consumer).authorized);
        SessionTarget retargeted; retargeted.host = "127.0.0.66"; retargeted.username = "synthetic-user";
        std::optional<DocumentReply> reply; files->lookup({id, Consumer}, retargeted, [&](DocumentReply value) { reply.emplace(std::move(value)); });
        QTRY_VERIFY(reply.has_value()); QVERIFY(reply->result.ok && !reply->password);
    }
    void changedInheritedAccountCannotReusePassword_data() {
        QTest::addColumn<QString>("field"); QTest::newRow("username") << QString("username"); QTest::newRow("domain") << QString("domain");
    }
    void changedInheritedAccountCannotReusePassword() {
        QFETCH(QString, field); QTemporaryDir dir; const auto path = dir.filePath("account.xml"); QVERIFY(fixture(path, "127.0.0.1"));
        MainWindow window(path, true); window.show(); auto* files = window.findChild<DocumentService*>(); const auto id = window.connectionFiles().first();
        QTRY_VERIFY(files->status(id) && !files->status(id)->busy); window.unlockConnectionFile(id, Secret(QByteArray("synthetic file master")));
        QTRY_VERIFY(files->status(id)->unlocked && !files->status(id)->busy); QVERIFY(window.selectConnectionProfile({id, Folder}));
        setGridText(window, "password", "synthetic-account-password"); acceptReview(window); QVERIFY(settled(window, id));
        QVERIFY(files->status(id)->passwords.value(Consumer).authorized); setGridText(window, field, "changed-account"); QVERIFY(settled(window, id));
        QVERIFY(files->status(id)->passwords.value(Consumer).present && !files->status(id)->passwords.value(Consumer).authorized);
        SessionTarget original; original.host = "127.0.0.1"; original.username = "synthetic-user"; auto changed = original;
        if (field == "username") changed.username = "changed-account"; else changed.domain = "changed-account";
        for (const auto& target : {original, changed}) {
            std::optional<DocumentReply> reply; files->lookup({id, Consumer}, target, [&](DocumentReply value) { reply.emplace(std::move(value)); });
            QTRY_VERIFY(reply.has_value()); QVERIFY(reply->result.ok && !reply->password);
        }
        QVERIFY(!contents(path).contains("synthetic-account-password")); assertSecretFree(window, "synthetic-account-password");
    }
    void temporaryAndDifferentCredentialsNeverPersist() {
        QTemporaryDir dir; const auto path = dir.filePath("transient.xml"); QVERIFY(fixture(path, "127.0.0.1"));
#if defined(Q_OS_UNIX) && defined(RM_WITH_RDP)
        PromptListener listener; QVERIFY(listener.port != 0);
#endif
        MainWindow window(path, true); window.show(); auto* files = window.findChild<DocumentService*>(); const auto id = window.connectionFiles().first();
        QTRY_VERIFY(files->status(id) && !files->status(id)->busy); window.unlockConnectionFile(id, Secret(QByteArray("synthetic file master")));
        QTRY_VERIFY(files->status(id)->unlocked && !files->status(id)->busy); QVERIFY(window.selectConnectionProfile({id, Consumer}));
#if defined(Q_OS_UNIX) && defined(RM_WITH_RDP)
        setGridPort(window, listener.port); QVERIFY(settled(window, id));
        const auto before = contents(path);
        for (bool temporary : {false, true}) {
            QTRY_VERIFY(!window.findChild<QDialog*>("CredentialsDialog"));
            if (temporary) window.quickConnect("127.0.0.1:" + QString::number(listener.port), false);
            else { auto* different = window.findChild<QAction*>("actionDifferentCredentials"); QVERIFY(different->isEnabled()); different->trigger(); }
            QTRY_VERIFY(window.findChild<QDialog*>("CredentialsDialog")); auto* prompt = window.findChild<QDialog*>("CredentialsDialog");
            QVERIFY(!prompt->findChild<QCheckBox*>("rememberPasswordCheck"));
            QVERIFY(!prompt->findChild<QCheckBox*>("clipboardCheck")->isChecked());
            auto* username = prompt->findChild<QLineEdit*>("usernameEdit"); QVERIFY(username); username->setFocus(); username->selectAll(); QTest::keyClicks(username, "session-only-user");
            auto* password = prompt->findChild<QLineEdit*>("passwordEdit"); QVERIFY(password); password->setFocus(); QTest::keyClicks(password, "synthetic-transient-password");
            QTest::mouseClick(prompt->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
            auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); QTRY_VERIFY2(tabs->count() == 1, qPrintable(window.lastError())); auto* pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane);
            QCOMPARE(pane->target().username, QString("session-only-user")); QCOMPARE(pane->profile().isEmpty(), temporary);
            if (temporary) QVERIFY(!pane->document().valid()); else QCOMPARE(pane->document(), id);
            window.closeTab(0); QCOMPARE(window.sessionCount(), size_t(0)); QCOMPARE(contents(path), before);
            QVERIFY(!QFile::exists(path + ".vault")); QVERIFY(!files->status(id)->passwords.value(Consumer).present);
            assertSecretFree(window, "synthetic-transient-password");
        }
#else
        const auto before = contents(path); PromptCredentialProvider provider; std::optional<TransientCredentials> credentials;
        auto* prompt = provider.request(&window, {"127.0.0.1:3389", "synthetic-user", true, {}}, [&](auto value) { credentials = std::move(value); });
        QVERIFY(!prompt->findChild<QCheckBox*>("rememberPasswordCheck"));
        auto* password = prompt->findChild<QLineEdit*>("passwordEdit"); password->setFocus(); QTest::keyClicks(password, "synthetic-transient-password");
        QTest::mouseClick(prompt->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
        QVERIFY(credentials && !credentials->clipboard); QCOMPARE(credentials->username, QString("synthetic-user"));
        QVERIFY(credentials->password == QByteArray("synthetic-transient-password")); credentials.reset(); QCOMPARE(contents(path), before);
        QVERIFY(!QFile::exists(path + ".vault"));
#endif
    }
    void passwordSaveFailureBlocksContinuation() {
        QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml"); QVERIFY(fixture(a, "127.0.0.1") && fixture(b, "127.0.0.2"));
        MainWindow window(a, true); window.show(); auto* files = window.findChild<DocumentService*>();
        const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
        QTRY_VERIFY(files->status(idA) && !files->status(idA)->busy && files->status(idB) && !files->status(idB)->busy);
        window.unlockConnectionFile(idA, Secret(QByteArray("synthetic file master"))); QTRY_VERIFY(files->status(idA)->unlocked && !files->status(idA)->busy);
        QVERIFY(window.selectConnectionProfile({idA, Folder})); const auto before = contents(a), beforeB = contents(b);
        bool done = false; files->setFault(idA, AtomicFile::Fault::BeforeReplace, [&](DocumentReply value) { done = value.result.ok; }); QTRY_VERIFY(done);
        setGridText(window, "password", "synthetic-retry-password"); acceptReview(window); QTRY_VERIFY(!window.lastError().isEmpty() && !files->status(idA)->busy);
        QCOMPARE(contents(a), before); QVERIFY(window.findChild<ProfileEditor*>()->dirty()); assertSecretFree(window, "synthetic-retry-password");
        window.selectConnectionFile(idB); QTRY_VERIFY(window.findChild<QMessageBox*>("saveRecovery"));
        auto* recovery = window.findChild<QMessageBox*>("saveRecovery"); QTest::mouseClick(recovery->button(QMessageBox::Cancel), Qt::LeftButton);
        QCOMPARE(window.selectedDocument(), idA); QCOMPARE(contents(b), beforeB);
        done = false; files->setFault(idA, AtomicFile::Fault::None, [&](DocumentReply value) { done = value.result.ok; }); QTRY_VERIFY(done);
        window.saveSelected(); QTRY_VERIFY(window.findChild<QMessageBox*>("saveRecovery")); recovery = window.findChild<QMessageBox*>("saveRecovery");
        QTest::mouseClick(recovery->button(QMessageBox::Retry), Qt::LeftButton); acceptReview(window); QVERIFY(settled(window, idA));
        QVERIFY(files->status(idA)->passwords.value(Consumer).authorized); QCOMPARE(contents(b), beforeB);
    }
    void secretDurabilityWarningGatesCapturedContinuation_data() {
        QTest::addColumn<bool>("lockAfterReplace"); QTest::newRow("acknowledge owning continuation") << false; QTest::newRow("lock cancels continuation") << true;
    }
    void secretDurabilityWarningGatesCapturedContinuation() {
        QFETCH(bool, lockAfterReplace); QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
        QVERIFY(fixture(a, "127.0.0.1") && fixture(b, "127.0.0.2")); Barrier barrier; barrier.after = true;
        MainWindow window(a, true); Unblock unblock{barrier}; window.show(); auto* files = window.findChild<DocumentService*>();
        const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
        QTRY_VERIFY(files->status(idA) && !files->status(idA)->busy && files->status(idB) && !files->status(idB)->busy);
        for (const auto id : {idA, idB}) {
            window.unlockConnectionFile(id, Secret(QByteArray("synthetic file master"))); QTRY_VERIFY(files->status(id)->unlocked && !files->status(id)->busy);
        }
        QVERIFY(window.selectConnectionProfile({idA, Consumer})); const auto before = contents(a), beforeB = contents(b);
        bool done = false; files->setFault(idA, AtomicFile::Fault::DirectorySync, [&](DocumentReply value) { done = value.result.ok; }); QTRY_VERIFY(done);
        done = false; files->setCommitGate(idA, barrier.gate(), [&](DocumentReply value) { done = value.result.ok; }); QTRY_VERIFY(done);
        setGridText(window, "password", "synthetic-warning-password"); window.connectSelected(true); acceptReview(window);
        QTRY_COMPARE(barrier.entered.available(), 1); if (lockAfterReplace) files->lock(idA); barrier.release.release();
        QTRY_VERIFY(window.findChild<QMessageBox*>("durabilityWarning")); QVERIFY(settled(window, idA));
        auto* warning = window.findChild<QMessageBox*>("durabilityWarning"); QVERIFY(!warning->text().contains("synthetic-warning-password"));
        QVERIFY(!files->status(idA)->warning.isEmpty()); QVERIFY(files->status(idA)->passwords.value(Consumer).present); QCOMPARE(window.sessionCount(), size_t(0));
        QVERIFY(contents(a) != before); QVERIFY(!contents(a).contains("synthetic-warning-password")); QCOMPARE(contents(b), beforeB);
        std::optional<DocumentReply> blocked; SessionTarget target; target.host = "127.0.0.1"; target.username = "synthetic-user";
        files->lookup({idA, Consumer}, target, [&](DocumentReply value) { blocked.emplace(std::move(value)); });
        QTRY_VERIFY(blocked.has_value()); QVERIFY(!blocked->result.ok && !blocked->password);
        QVERIFY(window.selectConnectionProfile({idB, Consumer})); setGridText(window, "username", "foreign-file-user"); QVERIFY(settled(window, idB));
        QVERIFY(contents(b) != beforeB); QVERIFY(files->status(idB)->warning.isEmpty()); QCOMPARE(window.selectedDocument(), idB);
        QTest::mouseClick(warning->button(QMessageBox::Ok), Qt::LeftButton); QTRY_VERIFY(files->status(idA)->warning.isEmpty());
        if (lockAfterReplace) {
            QCOMPARE(window.sessionCount(), size_t(0)); QVERIFY(!files->status(idA)->unlocked); QCOMPARE(window.selectedDocument(), idB);
        } else {
            QTRY_COMPARE(window.sessionCount(), size_t(1)); auto* tabs = window.findChild<QTabWidget*>("sessionTabs");
            auto* pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane); QVERIFY(pane->reference() == ProfileReference({idA, Consumer}));
            QCOMPARE(pane->target().host, QString("127.0.0.1")); QCOMPARE(pane->target().username, QString("synthetic-user")); window.closeTab(0);
        }
        auto saved = ConnectionXml::read(contents(a)); QVERIFY(saved); Secret master(QByteArray("synthetic file master")); QVERIFY(saved->unlock(master));
        auto password = saved->password(master, Consumer, target); QVERIFY(password && password->takeBytes() == QByteArray("synthetic-warning-password"));
        assertSecretFree(window, "synthetic-warning-password");
    }
    void lockAndCloseReconcileSecretCommit() {
        for (bool after : {false, true}) {
            QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
            QVERIFY(fixture(a, "127.0.0.1") && fixture(b, "127.0.0.2")); Barrier barrier; barrier.after = after;
            MainWindow window(a, true); Unblock unblock{barrier}; window.show(); auto* files = window.findChild<DocumentService*>();
            const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
            QTRY_VERIFY(files->status(idA) && !files->status(idA)->busy && files->status(idB) && !files->status(idB)->busy);
            window.unlockConnectionFile(idA, Secret(QByteArray("synthetic file master"))); QTRY_VERIFY(files->status(idA)->unlocked && !files->status(idA)->busy);
            QVERIFY(window.selectConnectionProfile({idA, Folder})); const auto before = contents(a), beforeB = contents(b);
            bool done = false; files->setCommitGate(idA, barrier.gate(), [&](DocumentReply value) { done = value.result.ok; }); QTRY_VERIFY(done);
            setGridText(window, "password", "synthetic-lock-password"); acceptReview(window); QTRY_COMPARE(barrier.entered.available(), 1);
            window.close(); QVERIFY(window.isVisible()); files->lock(idA);
            QVERIFY(!files->status(idA)->unlocked); QVERIFY(window.findChild<ProfileEditor*>()->passwordPending());
            assertSecretFree(window, "synthetic-lock-password"); barrier.release.release(); QTRY_VERIFY(!files->status(idA)->busy);
            QCOMPARE(files->status(idA)->passwords.value(Consumer).present, after);
            QVERIFY(!files->status(idA)->unlocked); QCOMPARE(contents(b), beforeB);
            if (after) {
                QVERIFY(contents(a) != before); QTRY_VERIFY(!window.findChild<ProfileEditor*>()->dirty()); QVERIFY(window.isVisible());
                QVERIFY(window.selectConnectionFile(idB)); QCOMPARE(window.selectedDocument(), idB);
            } else {
                QCOMPARE(contents(a), before); QVERIFY(window.findChild<ProfileEditor*>()->dirty()); QVERIFY(window.isVisible());
                window.selectConnectionFile(idB); QTRY_VERIFY(window.findChild<QMessageBox*>("saveRecovery"));
                auto* recovery = window.findChild<QMessageBox*>("saveRecovery"); QTest::mouseClick(recovery->button(QMessageBox::Discard), Qt::LeftButton);
                QTRY_COMPARE(window.selectedDocument(), idB); QVERIFY(!window.findChild<ProfileEditor*>()->dirty());
            }
            QVERIFY(!contents(a).contains("synthetic-lock-password"));
        }
    }
    void passwordWorkerRejectsInvalidEditsWithoutChangingSavedSecret() {
        QTemporaryDir dir; const auto path = dir.filePath("invalid-edits.xml"); QVERIFY(fixture(path, "127.0.0.1"));
        Secret master(QByteArray("synthetic file master")); auto xml = ConnectionXml::read(contents(path), &master); QVERIFY(xml);
        const QString sibling = "33333333-3333-4333-8333-333333333333";
        auto projection = QJsonDocument::fromJson(*xml->projection()).object(); auto profiles = projection["profiles"].toArray();
        profiles.append(QJsonObject{{"id", sibling}, {"name", "Unrelated"}, {"kind", "connection"}, {"parent", QJsonValue()},
            {"overrides", QJsonObject{{"host", "127.0.0.1"}, {"username", "synthetic-user"}, {"protocol", "rdp"}, {"port", 3389}}}});
        projection["profiles"] = profiles; QVERIFY(xml->patch(master, QJsonDocument(projection).toJson()));
        SessionTarget target; target.host = "127.0.0.1"; target.username = "synthetic-user";
        QVERIFY(xml->setPassword(master, Folder, Secret(QByteArray("synthetic-existing-password"))));
        QVERIFY(xml->authorizePasswords(master, Folder, {{Consumer, target}})); const auto bytes = xml->serialize(master); QVERIFY(bytes);
        QFile output(path); QVERIFY(output.open(QIODevice::WriteOnly)); QCOMPARE(output.write(*bytes), bytes->size()); output.close(); const auto before = contents(path);
        DocumentService files; std::optional<DocumentReply> opened; const auto id = files.open(path, [&](DocumentReply value) { opened.emplace(std::move(value)); });
        QTRY_VERIFY(opened.has_value()); QVERIFY(opened->result.ok); const auto metadata = opened->projection;
        std::optional<DocumentReply> unlocked; files.unlock(id, master.clone(), [&](DocumentReply value) { unlocked.emplace(std::move(value)); });
        QTRY_VERIFY(unlocked.has_value()); QVERIFY(unlocked->result.ok);
        for (const QByteArray& invalid : {QByteArray(), QByteArray(4097, 'x'), QByteArray::fromHex("c080")}) {
            std::optional<DocumentReply> rejected;
            files.editPassword({id, Folder}, metadata, PasswordEdit{PasswordEdit::Kind::Set, Secret(invalid), false}, {{Consumer, target}}, {}, {},
                [&](DocumentReply value) { rejected.emplace(std::move(value)); });
            QTRY_VERIFY(rejected.has_value()); QVERIFY(!rejected->result.ok && !rejected->committed && !rejected->password);
            QCOMPARE(contents(path), before); QVERIFY(files.status(id)->passwords.value(Consumer).authorized);
        }
        for (bool unrelatedSource : {false, true}) {
            std::optional<DocumentReply> rejected; std::optional<Secret> replacement;
            if (!unrelatedSource) replacement.emplace(QByteArray("mR3m"));
            files.editPassword({id, Folder}, metadata, PasswordEdit{PasswordEdit::Kind::Set, Secret(QByteArray("synthetic-rejected-password")), false},
                {{unrelatedSource ? sibling : Consumer, target}}, std::move(replacement), {}, [&](DocumentReply value) { rejected.emplace(std::move(value)); });
            QTRY_VERIFY(rejected.has_value()); QVERIFY(!rejected->result.ok && !rejected->committed);
            QCOMPARE(contents(path), before); QVERIFY(files.status(id)->passwords.value(Consumer).authorized);
        }
        std::optional<DocumentReply> lookup; files.lookup({id, Consumer}, target, [&](DocumentReply value) { lookup.emplace(std::move(value)); });
        QTRY_VERIFY(lookup.has_value()); QVERIFY(lookup->result.ok && lookup->password);
        QVERIFY(lookup->password->takeBytes() == QByteArray("synthetic-existing-password"));
    }
    void firstSecretSavesUnderDefaultWithoutMasterDialog() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); MainWindow window(path, true); window.show();
        auto* files = window.findChild<DocumentService*>(); const auto id = window.connectionFiles().first(); QTRY_VERIFY(files->status(id) && !files->status(id)->busy);
        window.addConnection(); QVERIFY(settled(window, id)); const auto profile = window.selectedId();
        const auto before = contents(path); QVERIFY(files->status(id)->compatibilityMaster);
        bool done = false; files->setFault(id, AtomicFile::Fault::BeforeReplace, [&](DocumentReply value) { done = value.result.ok; }); QTRY_VERIFY(done);
        setGridText(window, "password", "synthetic-config-password"); QTRY_VERIFY(!files->status(id)->busy && !window.lastError().isEmpty());
        QVERIFY(!window.findChild<QDialog*>("filePassword"));
        QCOMPARE(contents(path), before); QVERIFY(files->status(id)->compatibilityMaster && !files->status(id)->passwords.value(profile).present);
        QVERIFY(window.findChild<ProfileEditor*>()->dirty()); assertSecretFree(window, "synthetic-config-password");
        done = false; files->setFault(id, AtomicFile::Fault::None, [&](DocumentReply value) { done = value.result.ok; }); QTRY_VERIFY(done);
        window.saveSelected(); QTRY_VERIFY(window.findChild<QMessageBox*>("saveRecovery"));
        QTest::mouseClick(window.findChild<QMessageBox*>("saveRecovery")->button(QMessageBox::Retry), Qt::LeftButton); QVERIFY(settled(window, id));
        QVERIFY(files->status(id)->compatibilityMaster && files->status(id)->passwordProtected == false); QVERIFY(files->status(id)->passwords.value(profile).present);
        QVERIFY(!window.findChild<QDialog*>("filePassword"));
        const auto bytes = contents(path); QVERIFY(!bytes.contains("synthetic-config-password")); QVERIFY(!bytes.contains("synthetic file master"));
        const Secret initial(QByteArray("mR3m")); auto saved = ConnectionXml::read(bytes, &initial); QVERIFY(saved && saved->passwordStates().value(profile).local);
        assertSecretFree(window, "synthetic-config-password");
        QCOMPARE(gridRow(window, "password")->text(1), QString(8, QChar(0x2022)));
        auto* tree = window.findChild<QTreeWidget*>("configTree"); auto* password = gridRow(window, "password");
        tree->setCurrentItem(password, 1); tree->scrollToItem(password);
        const QPoint point(tree->columnViewportPosition(1) + tree->columnWidth(1) / 2, tree->visualItemRect(password).center().y());
        QContextMenuEvent event(QContextMenuEvent::Mouse, point, tree->viewport()->mapToGlobal(point)); QApplication::sendEvent(tree->viewport(), &event);
        QTRY_VERIFY(window.findChild<QMenu*>("configContextMenu")); auto* menu = window.findChild<QMenu*>("configContextMenu");
        auto* editAction = menu->findChild<QAction*>("actionEditPassword"); auto* clear = menu->findChild<QAction*>("actionClearPassword");
        QVERIFY(editAction && clear && editAction->isEnabled() && clear->isEnabled());
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(clear).center());
        QVERIFY(settled(window, id)); QVERIFY(!files->status(id)->passwords.value(profile).present);
    }
    void fileMasterDialogAcceptsCompatibleShortAndLatin1Passwords_data() {
        QTest::addColumn<QString>("input"); QTest::addColumn<bool>("accepted");
        QTest::newRow("one character") << QString("x") << true;
        QTest::newRow("Latin-1") << QString::fromUtf8("synthetic-\xc3\xa9") << true;
        QTest::newRow("4096 Latin-1 characters") << QString(4096, QChar(0xe9)) << true;
        QTest::newRow("4097 characters") << QString(4097, QChar(0xe9)) << false;
        QTest::newRow("whitespace") << QString(" \t\n") << false;
        QTest::newRow("public default") << QString("mR3m") << false;
        QTest::newRow("outside single-byte") << QString(QChar(0x100)) << false;
        QTest::newRow("empty") << QString() << false;
        QTest::newRow("NUL") << QString(QChar(0)) << false;
    }
    void fileMasterDialogAcceptsCompatibleShortAndLatin1Passwords() {
        QFETCH(QString, input); QFETCH(bool, accepted); QWidget owner; owner.show(); bool done = false; std::optional<Secret> returned;
        auto* dialog = FilePasswordDialogs::passphrase(&owner, FilePasswordDialogs::Mode::ChangeMaster,
            [&](std::optional<Secret> value) { done = true; returned = std::move(value); });
        auto* first = dialog->findChild<QLineEdit*>("masterEdit"); auto* second = dialog->findChild<QLineEdit*>("masterConfirmEdit");
        QVERIFY(first && second); QCOMPARE(first->echoMode(), QLineEdit::Password); QCOMPARE(second->echoMode(), QLineEdit::Password);
        first->setText(input); second->setText(input); QVERIFY(first->text() == input && second->text() == input);
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        if (accepted) { QVERIFY(done && returned); QVERIFY(returned->takeBytes() == input.toUtf8()); }
        else { QVERIFY(!done && dialog->isVisible()); QVERIFY(!dialog->findChild<QLabel*>("errorLabel")->text().isEmpty()); dialog->reject(); QVERIFY(done && !returned); }
        QVERIFY(first->text().isEmpty() && second->text().isEmpty());
    }
    void fileMasterConfirmationRequiresExactMatch() {
        QWidget owner; owner.show(); bool done = false;
        auto* dialog = FilePasswordDialogs::passphrase(&owner, FilePasswordDialogs::Mode::ChangeMaster,
            [&](std::optional<Secret> value) { done = value.has_value(); });
        auto* first = dialog->findChild<QLineEdit*>("masterEdit"); auto* second = dialog->findChild<QLineEdit*>("masterConfirmEdit");
        first->setText("x "); second->setText("x"); dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        QVERIFY(!done && dialog->isVisible()); second->setText("x "); dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click(); QVERIFY(done);
    }
    void lockedReconnectUnlocksCapturedOwner_data() {
        QTest::addColumn<bool>("defaultMaster"); QTest::newRow("custom prompt") << false; QTest::newRow("default automatic") << true;
    }
    void lockedReconnectUnlocksCapturedOwner() {
#if defined(Q_OS_UNIX) && defined(RM_WITH_RDP)
        QFETCH(bool, defaultMaster); PromptListener listener; QVERIFY(listener.port != 0);
        QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml"); QVERIFY(fixture(a, "127.0.0.1") && fixture(b, "127.0.0.1"));
        MainWindow window(a, true); window.show(); auto* files = window.findChild<DocumentService*>(); const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
        QTRY_VERIFY(files->status(idA) && !files->status(idA)->busy && files->status(idB) && !files->status(idB)->busy);
        for (auto id : {idA, idB}) { window.unlockConnectionFile(id, Secret(QByteArray("synthetic file master"))); QTRY_VERIFY(files->status(id)->unlocked && !files->status(id)->busy); }
        QVERIFY(window.selectConnectionProfile({idA, Consumer})); setGridPort(window, listener.port); QVERIFY(settled(window, idA));
        QVERIFY(window.selectConnectionProfile({idA, Folder})); setGridText(window, "password", "synthetic-reconnect-password"); acceptReview(window); QVERIFY(settled(window, idA));
        if (defaultMaster) {
            std::optional<DocumentReply> reset; files->resetMaster(idA, {}, [&](DocumentReply value) { reset.emplace(std::move(value)); });
            QTRY_VERIFY(reset.has_value()); QVERIFY(reset->result.ok && reset->committed);
        }
        QVERIFY(window.selectConnectionProfile({idA, Consumer})); window.connectSelected(false); QTRY_COMPARE(window.sessionCount(), size_t(1));
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); auto* original = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(original); const auto oldSession = original->id();
        window.disconnectCurrent(); QVERIFY(window.selectConnectionFile(idA)); window.findChild<QAction*>("actionLockPasswords")->trigger();
        QVERIFY(!files->status(idA)->unlocked && files->status(idB)->unlocked); QVERIFY(window.selectConnectionFile(idB)); const auto beforeA = contents(a), beforeB = contents(b);
        window.reconnectCurrent();
        if (!defaultMaster) {
            QTRY_VERIFY(window.findChild<QDialog*>("filePassword")); QVERIFY(window.findChild<QDialog*>("filePassword")->findChild<QLabel*>("hintLabel")->text().contains(a));
            window.findChild<QDialog*>("filePassword")->reject(); QTRY_VERIFY(!window.findChild<QDialog*>("filePassword"));
            QCOMPARE(window.sessionCount(), size_t(1)); QCOMPARE(qobject_cast<SessionPane*>(tabs->widget(0))->id(), oldSession); QVERIFY(!window.findChild<QDialog*>("CredentialsDialog"));
            window.reconnectCurrent(); QTRY_VERIFY(window.findChild<QDialog*>("filePassword")); auto* dialog = window.findChild<QDialog*>("filePassword");
            dialog->findChild<QLineEdit*>("masterEdit")->setText("synthetic file master"); dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        }
        QTRY_VERIFY(files->status(idA)->unlocked && qobject_cast<SessionPane*>(tabs->widget(0))->id() != oldSession);
        auto* replacement = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(replacement->reference() == ProfileReference({idA, Consumer})); QCOMPARE(replacement->target().port, listener.port);
        QCOMPARE(window.selectedDocument(), idB); QVERIFY(!window.findChild<QDialog*>("filePassword") && !window.findChild<QDialog*>("CredentialsDialog"));
        QCOMPARE(contents(a), beforeA); QCOMPARE(contents(b), beforeB); QCOMPARE(window.sessionCount(), size_t(1)); window.closeTab(0);
#else
        QSKIP("Requires the owned loopback listener and embedded RDP worker.");
#endif
    }
    void connectDuringNativeReviewRetainsCapturedContinuation() {
#if defined(Q_OS_UNIX) && defined(RM_WITH_RDP)
        PromptListener listener; QVERIFY(listener.port != 0); QTemporaryDir dir; const auto path = dir.filePath("native.xml");
        QVERIFY(fixture(path, "127.0.0.1", listener.port)); const Secret master(QByteArray("synthetic file master"));
        auto native = ConnectionXml::read(contents(path), &master); QVERIFY(native && native->credentialReviewRequired());
        QVERIFY(native->setPassword(master, Folder, Secret(QByteArray("synthetic-import-password")))); const auto serialized = native->serialize(master); QVERIFY(serialized);
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(*serialized), serialized->size()); }
        const auto before = contents(path); MainWindow window(path, true); window.show(); const auto id = window.connectionFiles().first();
        QTRY_VERIFY(window.findChild<QDialog*>("filePassword")); auto* unlock = window.findChild<QDialog*>("filePassword");
        unlock->findChild<QLineEdit*>("masterEdit")->setText("synthetic file master"); unlock->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        QTRY_VERIFY(window.findChild<QMessageBox*>("nativePasswordReview")); QVERIFY(window.selectConnectionProfile({id, Consumer})); window.connectSelected(false);
        QCOMPARE(window.sessionCount(), size_t(0)); window.findChild<QMessageBox*>("nativePasswordReview")->button(QMessageBox::Yes)->click();
        QTRY_COMPARE(window.sessionCount(), size_t(1)); QVERIFY(!window.findChild<QDialog*>("CredentialsDialog"));
        auto* pane = window.findChild<SessionPane*>(); QVERIFY(pane && pane->reference() == ProfileReference({id, Consumer})); QCOMPARE(pane->target().port, listener.port);
        QCOMPARE(contents(path), before); window.closeTab(0);
#else
        QSKIP("Requires the owned loopback listener and embedded RDP worker.");
#endif
    }
    void transientCredentialPromptNeverStoresPasswords() {
        QTemporaryDir dir; MainWindow window(dir.filePath("confCons.xml"), true); window.show();
        const auto id = window.connectionFiles().first(); auto* files = window.findChild<DocumentService*>(); QTRY_VERIFY(files->status(id) && !files->status(id)->busy);
        const auto before = contents(dir.filePath("confCons.xml")); bool canceled = false; PromptCredentialProvider provider;
        auto* prompt = provider.request(&window, {"127.0.0.1:3389", "synthetic-user", true, {}},
            [&](std::optional<TransientCredentials> result) { canceled = !result; });
        QVERIFY(!prompt->findChild<QCheckBox*>("rememberPasswordCheck"));
        QVERIFY(!prompt->findChild<QCheckBox*>("clipboardCheck")->isChecked()); prompt->reject(); QVERIFY(canceled);
        QCOMPARE(contents(dir.filePath("confCons.xml")), before);
    }
    void configPasswordKeepsRolesAndDraftsSecretFree() {
        QTemporaryDir dir; MainWindow window(dir.filePath("confCons.xml"), true); window.show();
        auto* service = window.findChild<DocumentService*>(); QVERIFY(service);
        const auto document = window.connectionFiles().first(); QTRY_VERIFY(service->status(document) && !service->status(document)->busy);
        window.addConnection(); QTRY_VERIFY(!window.selectedId().isEmpty() && !service->status(document)->busy);
        auto* password = row(window, "password"); auto* username = row(window, "username"); auto* domain = row(window, "domain");
        QVERIFY(password && username && domain); QCOMPARE(password->parent(), username->parent());
        const auto* group = password->parent(); QVERIFY(group);
        QCOMPARE(group->indexOfChild(password), group->indexOfChild(username) + 1);
        QCOMPARE(group->indexOfChild(domain), group->indexOfChild(password) + 1);
        QVERIFY(password->flags() & Qt::ItemIsEditable);
        const auto draft = window.findChild<ProfileEditor*>()->draft();
        QVERIFY(!draft.contains("password") && !draft["overrides"].toObject().contains("password"));
    }
};
QTEST_MAIN(PasswordEditingTest)
#include "password_editing.moc"
