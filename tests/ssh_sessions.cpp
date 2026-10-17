#include "MainWindow.h"
#include "SessionPane.h"
#include "SshCredentialsDialog.h"
#include "SshWorker.h"
#include "ConnectionXml.h"
#include "ProfileEditor.h"
#include "GridDriver.h"
#include "Views.h"
#include "TerminalScreen.h"
#include "ConnectionTree.h"
#include "SshHostKeyService.h"
#include "ClipboardDriver.h"
#include <QAction>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QMenu>
#include <QTimer>

namespace {
const QString profileId = "e6683f1f-4e06-4200-9a17-89cd30c68b12";
QString fixtureDirectory() {
    const auto build = qEnvironmentVariable("RM_BUILD_DIR");
    if (build.isEmpty()) qFatal("Run SSH session tests through scripts/env.py.");
    return build + "/test-data/ssh-sessions-XXXXXX";
}
bool fixture(const QString& path, quint16 port, QString auth = "password", QString keyPath = {}) {
    auto xml = ConnectionXml::create(); Secret master(QByteArray("mR3m"));
    const QJsonObject profile{{"id", profileId}, {"name", "Owned SSH"}, {"kind", "connection"}, {"parent", QJsonValue()},
        {"overrides", QJsonObject{{"host", "127.0.0.1"}, {"username", "TEAM\\literal"}, {"domain", "inactive domain"},
            {"protocol", "ssh"}, {"port", port}, {"ssh_auth", auth}, {"ssh_key_path", keyPath}}}};
    const auto projection = QJsonDocument(QJsonObject{{"schema_version", 2}, {"profiles", QJsonArray{profile}}}).toJson(QJsonDocument::Compact);
    if (!xml || !xml->unlock(master) || !xml->patch(master, projection)) return false;
    const auto output = xml->serialize(master); QFile file(path);
    return output && file.open(QIODevice::WriteOnly) && file.write(*output) == output->size();
}
void acceptSyntheticCredentials(SshCredentialsDialog* dialog) {
    dialog->findChild<QLineEdit*>("authenticationEdit")->setText("synthetic-session-password");
    dialog->findChild<QDialogButtonBox*>("buttonBox")->button(QDialogButtonBox::Ok)->click();
}
}
class SshSessionsTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(QDir().mkpath(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data")); }
    void cleanup() { QTRY_COMPARE_WITH_TIMEOUT(SshWorker::activeWorkers(), 0, 3000); }
    void sshCredentialCancelPreventsProductionRegistration() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid());
        const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path, 22));
        MainWindow window(path, true, nullptr, {}, directory.filePath("ssh-host-keys.json")); window.show();
        QTRY_VERIFY(window.selectConnectionProfile({window.connectionFiles().first(), profileId}));
        window.connectSelected();
        QTRY_VERIFY(window.findChild<SshCredentialsDialog*>());
        QCOMPARE(window.sessionCount(), size_t(0));
        window.findChild<SshCredentialsDialog*>()->reject();
        QCOMPARE(window.sessionCount(), size_t(0)); QVERIFY(window.findChildren<SessionPane*>().isEmpty());
    }
    void sshQuickConnectSelectorDefaultsAndNeverPersists() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid());
        MainWindow window(directory.filePath("connections.xml"), true); window.show();
        QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        QCOMPARE(window.quickConnectProtocol(), QString("rdp"));
        // Choose SSH from the RDP button's arrow menu, as a user would.
        auto* menu = window.findChild<QMenu*>("quickConnectProtocols"); QVERIFY(menu);
        for (auto* action : menu->actions()) if (action->text() == "SSH") action->trigger();
        QCOMPARE(window.quickConnectProtocol(), QString("ssh")); window.quickConnect("localhost", true);
        auto* pane = window.findChild<SessionPane*>(); QVERIFY(pane);
        QCOMPARE(pane->target().protocol, QString("ssh")); QCOMPARE(pane->target().port, quint16(22));
        QVERIFY(pane->profile().isEmpty()); QCOMPARE(window.sessionCount(), size_t(1));
        window.closeTab(0); QCOMPARE(window.sessionCount(), size_t(0));
        window.quickConnect("[::1]:2222", true); pane = window.findChild<SessionPane*>(); QVERIFY(pane);
        QCOMPARE(pane->target().host, QString("::1")); QCOMPARE(pane->target().port, quint16(2222));
    }
    void sshPrivateKeyPromptBypassesLockedSavedPasswords() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid());
        const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path, 22, "private_key", directory.filePath("owned-key")));
        MainWindow window(path, true, nullptr, {}, directory.filePath("ssh-host-keys.json")); window.show();
        QTRY_VERIFY(window.selectConnectionProfile({window.connectionFiles().first(), profileId}));
        window.findChild<QAction*>("actionLockPasswords")->trigger();
        window.connectSelected(); QTRY_VERIFY(window.findChild<SshCredentialsDialog*>());
        auto* dialog = window.findChild<SshCredentialsDialog*>();
        QVERIFY(!dialog->findChild<QComboBox*>("authCombo")->isEnabled());
        QVERIFY(dialog->findChild<QLineEdit*>("keyPathEdit")->isReadOnly());
        QCOMPARE(dialog->findChild<QComboBox*>("authCombo")->currentData().toString(), QString("private_key"));
        QVERIFY(!window.findChild<QDialog*>("filePasswordDialog")); QCOMPARE(window.sessionCount(), size_t(0)); dialog->reject();
    }
    void savedSshReconnectResolvesOwningFileAndCopiedIdsStayIndependent() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid());
        const auto a = directory.filePath("a.xml"), b = directory.filePath("b.xml"); QVERIFY(fixture(a, 2222) && fixture(b, 2223));
        MainWindow window(a, true); window.show();
        const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
        QTRY_VERIFY(window.selectConnectionProfile({idA, profileId})); window.connectSelected(true);
        auto* original = window.findChild<SessionPane*>(); QVERIFY(original); const auto originalSession = original->id();
        setGridText(window, "host", "127.0.0.2");
        QTRY_VERIFY(!window.findChild<ProfileEditor*>()->dirty()); QCOMPARE(original->target().host, QString("127.0.0.1"));
        QTRY_VERIFY(window.selectConnectionProfile({idB, profileId})); window.reconnectCurrent();
        original = window.findChild<SessionPane*>(); QVERIFY(original); QCOMPARE(original->document(), idA);
        QCOMPARE(original->target().host, QString("127.0.0.2")); QVERIFY(original->id() != originalSession);
        window.connectSelected(true); QCOMPARE(window.sessionCount(), size_t(2));
        const auto panes = window.findChildren<SessionPane*>(); QVERIFY(panes[0]->document() != panes[1]->document());
        QCOMPARE(panes[1]->target().port, quint16(2223));
    }
    void sshCredentialCapturedOwnerRejectsStaleAttach() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid());
        const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path, 22));
        MainWindow window(path, true, nullptr, {}, directory.filePath("ssh-host-keys.json")); window.show();
        QTRY_VERIFY(window.selectConnectionProfile({window.connectionFiles().first(), profileId}));
        window.connectSelected(); QTRY_VERIFY(window.findChild<SshCredentialsDialog*>());
        QPointer<SshCredentialsDialog> dialog = window.findChild<SshCredentialsDialog*>();
        setGridText(window, "host", "127.0.0.2"); QTRY_VERIFY(!window.findChild<ProfileEditor*>()->dirty());
        QTRY_VERIFY(!dialog); QCOMPARE(window.sessionCount(), size_t(0));
    }
    void sshCredentialCancelAndLockedOwnerRejectStaleAttach() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid());
        const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path, 22));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto original = file.readAll(); file.close();
        MainWindow window(path, true, nullptr, std::make_shared<TestClipboardEndpoint>(), directory.filePath("trust.json")); window.show();
        const auto document = window.connectionFiles().first();
        QTRY_VERIFY(window.selectConnectionProfile({document, profileId}));
        window.connectSelected(); QTRY_VERIFY(window.findChild<SshCredentialsDialog*>());
        window.findChild<SshCredentialsDialog*>()->reject(); QCOMPARE(window.sessionCount(), size_t(0));
        window.connectSelected(); QTRY_VERIFY(window.findChild<SshCredentialsDialog*>());
        QPointer<SshCredentialsDialog> stale = window.findChild<SshCredentialsDialog*>();
        window.findChild<DocumentService*>()->lock(document);
        QTRY_VERIFY(!stale); QCOMPARE(window.sessionCount(), size_t(0)); QVERIFY(window.findChildren<SessionPane*>().isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), original);
    }
    void sshEditsAndMovesDoNotRetargetRunningSession() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid());
        const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path, 2222));
        MainWindow window(path, true, nullptr, std::make_shared<TestClipboardEndpoint>()); window.show();
        const auto document = window.connectionFiles().first(); QTRY_VERIFY(window.selectConnectionProfile({document, profileId}));
        window.connectSelected(true); auto* pane = window.findChild<SessionPane*>(); QVERIFY(pane);
        const auto session = pane->id(); const auto target = pane->target();
        setGridText(window, "host", "edited.invalid"); QVERIFY(waitGridSaved(window));
        QCOMPARE(pane->id(), session); QVERIFY(pane->target() == target);
        QVERIFY(window.selectConnectionFile(document)); window.addFolder(); QVERIFY(waitGridSaved(window));
        const auto folder = window.selectedId(); QVERIFY(!folder.isEmpty());
        auto* tree = window.findChild<ConnectionTree*>("connectionTree"); QVERIFY(tree);
        const ProfileReference source{document, profileId}, destination{document, folder};
        QVERIFY(tree->validMove(source, destination)); tree->scopedMoveRequested(source, destination);
        QTRY_VERIFY([&] {
            for (const auto& value : storedXml(path).value("profiles").toArray())
                if (value.toObject().value("id").toString() == profileId) return value.toObject().value("parent").toString() == folder;
            return false;
        }());
        QVERIFY(waitGridSaved(window)); QCOMPARE(pane->id(), session); QVERIFY(pane->target() == target);
        QCOMPARE(pane->document(), document); QCOMPARE(window.sessionCount(), size_t(1));
    }
    void sshOutputRejectsRetiredGeneration() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid());
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        rm::SessionInfo info{}; info.id = "owned-output-session"; info.protocol = "ssh"; info.host = "127.0.0.1";
        info.port = server.serverPort(); info.username = "literal"; info.ssh_auth = "password";
        auto clipboard = std::make_shared<TestClipboardEndpoint>();
        SshHostKeyService trust(directory.filePath("trust.json")); SessionPane pane(info, {}, nullptr, {}, clipboard);
        pane.resize(800, 480); pane.show(); QTest::qWait(10);
        auto* worker = pane.startSsh(sshPasswordAuthentication(Secret(QByteArray("synthetic-output-password"))), &trust); QVERIFY(worker);
        auto* view = pane.findChild<TerminalView*>(); QVERIFY(view && view->screen()); QVERIFY(view->screen()->cells().width() >= 7);
        worker->output("CURRENT", worker->generation()); QCOMPARE(view->screen()->lineText(0).trimmed(), QString("CURRENT"));
        QPointer<SshWorker> retired = worker;
        QVERIFY(QMetaObject::invokeMethod(worker, [retired] { if (retired) retired->output("RETIRED", retired->generation()); }, Qt::QueuedConnection));
        pane.shutdown(); QCoreApplication::sendPostedEvents();
        QCOMPARE(view->screen()->lineText(0).trimmed(), QString("CURRENT"));
        QVERIFY(!view->pasteClipboard()); QCOMPARE(clipboard->reads, 0); QCOMPARE(clipboard->writes, 0); QCOMPARE(clipboard->watches, 0);
        QTRY_COMPARE_WITH_TIMEOUT(SshWorker::activeWorkers(), 0, 3000);
    }
    void sshProductionWorkerDisconnectAndCloseStayResponsive() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid());
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path, server.serverPort()));
        MainWindow window(path, true, nullptr, {}, directory.filePath("ssh-host-keys.json")); window.show();
        QTRY_VERIFY(window.selectConnectionProfile({window.connectionFiles().first(), profileId}));
        auto* connectAction = window.findChild<QAction*>("actionTreeConnect"); QVERIFY(connectAction && connectAction->isEnabled());
        connectAction->trigger(); QTRY_VERIFY(window.findChild<SshCredentialsDialog*>());
        acceptSyntheticCredentials(window.findChild<SshCredentialsDialog*>());
        QTRY_COMPARE(window.sessionCount(), size_t(1));
        auto* pane = window.findChild<SessionPane*>(); QVERIFY(pane && !pane->mock());
        QVERIFY(pane->findChild<TerminalView*>()->screen()); QVERIFY(!pane->clipboardSharingAvailable());
        QCOMPARE(pane->target().username, QString("TEAM\\literal")); QVERIFY(pane->target().domain.isEmpty());
        QTRY_VERIFY(server.hasPendingConnections()); auto* socket = server.nextPendingConnection(); QVERIFY(socket);
        QSignalSpy closed(socket, &QTcpSocket::disconnected);
        int beats = 0; QTimer heartbeat; connect(&heartbeat, &QTimer::timeout, this, [&] { ++beats; }); heartbeat.start(1);
        QElapsedTimer elapsed; elapsed.start(); window.disconnectCurrent(); QVERIFY(elapsed.elapsed() < 100);
        QCOMPARE(window.sessionCount(), size_t(1)); QTRY_VERIFY(!closed.isEmpty()); QTRY_VERIFY(beats > 0);
        QTRY_COMPARE_WITH_TIMEOUT(SshWorker::activeWorkers(), 0, 3000);
        window.reconnectCurrent(); QTRY_VERIFY(window.findChild<SshCredentialsDialog*>());
        acceptSyntheticCredentials(window.findChild<SshCredentialsDialog*>());
        QTRY_VERIFY(server.hasPendingConnections()); socket = server.nextPendingConnection(); QVERIFY(socket);
        elapsed.restart(); window.close(); QVERIFY(elapsed.elapsed() < 100);
        QTRY_COMPARE_WITH_TIMEOUT(SshWorker::activeWorkers(), 0, 3000);
        QTRY_VERIFY(!window.isVisible()); QCOMPARE(window.sessionCount(), size_t(0));
    }
};
QTEST_MAIN(SshSessionsTest)
#include "ssh_sessions.moc"
