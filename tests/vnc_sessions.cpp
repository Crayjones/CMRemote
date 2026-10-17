#include "MainWindow.h"
#include "SessionPane.h"
#include "VncCredentialsDialog.h"
#include "VncDesktopView.h"
#include "VncWorker.h"
#include "ConnectionCrypto.h"
#include "ConnectionXml.h"
#include "ProfileEditor.h"
#include "GridDriver.h"
#include "ClipboardDriver.h"
#include "Views.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabBar>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QXmlStreamReader>

namespace {
const QString profileId = "e6683f1f-4e06-4200-9a17-89cd30c68b12";
const QString folderId = "1897a1fb-b702-45e9-bcae-884f59cc66c7";
QString fixtureDirectory() {
    const auto build = qEnvironmentVariable("RM_BUILD_DIR");
    if (build.isEmpty()) qFatal("Run VNC session tests through scripts/env.py.");
    return build + "/test-data/vnc-sessions-XXXXXX";
}
QByteArray contents(const QString& path) {
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool writeFixture(const QString& path, const QByteArray& bytes) {
    QFile file(path); return !bytes.isEmpty() && file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool fixture(const QString& path, quint16 port, bool viewOnly = false) {
    auto xml = ConnectionXml::create(); Secret master(QByteArray("mR3m"));
    const QJsonObject profile{{"id", profileId}, {"name", "Owned VNC"}, {"kind", "connection"}, {"parent", QJsonValue()},
        {"overrides", QJsonObject{{"host", "127.0.0.1"}, {"username", "TEAM\\inactive"}, {"domain", "inactive domain"},
            {"protocol", "vnc"}, {"port", port}, {"view_only", viewOnly}, {"resolution", "1920x1080"}, {"console_session", true}}}};
    const auto projection = QJsonDocument(QJsonObject{{"schema_version", 2}, {"profiles", QJsonArray{profile}}}).toJson(QJsonDocument::Compact);
    if (!xml || !xml->unlock(master) || !xml->patch(master, projection)) return false;
    const auto output = xml->serialize(master); return output && writeFixture(path, *output);
}
QByteArray nativeXml(const QByteArray& body) {
    const auto protectedValue = ConnectionCrypto::encrypt(Secret(QByteArray("mR3m")), 1000, Secret(QByteArray("ThisIsNotProtected")));
    if (!protectedValue) return {};
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Connections Name=\"Connections\" Export=\"false\" ConfVersion=\"2.8\" "
        "EncryptionEngine=\"AES\" BlockCipherMode=\"GCM\" KdfIterations=\"1000\" FullFileEncryption=\"false\" Protected=\"" +
        *protectedValue + "\">" + body + "</Connections>";
}
QByteArray nativeNode(quint16 port, const QByteArray& attributes = {}) {
    return "<Node Type=\"Connection\" Id=\"" + profileId.toLatin1() + "\" Name=\"Imported VNC\" Protocol=\"VNC\" "
        "Hostname=\"127.0.0.1\" Port=\"" + QByteArray::number(port) + "\" " + attributes + "/>";
}
QByteArray nodeAttribute(const QByteArray& xml, const QString& name, const QString& id = profileId) {
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == "Node" && reader.attributes().value("Id") == id)
            return reader.attributes().value(name).toLatin1();
    }
    return {};
}
quint16 closedLoopbackPort() {
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) return 0;
    const auto port = server.serverPort(); server.close(); return port;
}
void submitPassword(VncCredentialsDialog* dialog, const QString& password) {
    dialog->findChild<QLineEdit*>("passwordEdit")->setText(password);
    dialog->findChild<QDialogButtonBox*>("buttonBox")->button(QDialogButtonBox::Ok)->click();
}
// DES response for password "password" to challenge 0..15 (same vector as core/tests/vnc_transport.rs).
const QByteArray PasswordResponse = QByteArray::fromHex("b866924125c8eebb9debc1db61c538e2");
// Minimal owned RFB 3.8 server on loopback: offers VncAuth or None, then a
// 64×48 ServerInit, and never sends framebuffer updates.
class FakeRfb final : public QObject {
public:
    explicit FakeRfb(bool password) : password_(password) {
        server_.listen(QHostAddress::LocalHost);
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = server_.nextPendingConnection()) {
                ++accepted; socket_ = socket; stage_ = 0; buffer_.clear(); socket->write("RFB 003.008\n");
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { if (socket == socket_) { buffer_ += socket->readAll(); step(); } });
            }
        });
    }
    quint16 port() const { return server_.serverPort(); }
    int accepted = 0, selected = -1; QByteArray response; bool initialized = false;
private:
    bool take(qsizetype size, QByteArray& out) { if (buffer_.size() < size) return false; out = buffer_.left(size); buffer_.remove(0, size); return true; }
    void step() {
        QByteArray part;
        if (stage_ == 0 && take(12, part)) { socket_->write(password_ ? QByteArray("\x01\x02", 2) : QByteArray("\x01\x01", 2)); stage_ = 1; }
        if (stage_ == 1 && take(1, part)) {
            selected = part[0];
            if (selected == 2) { QByteArray challenge; for (int i = 0; i < 16; ++i) challenge += char(i); socket_->write(challenge); stage_ = 2; }
            else { socket_->write(QByteArray(4, '\0')); stage_ = 3; }
        }
        if (stage_ == 2 && take(16, part)) {
            response = part;
            if (part == PasswordResponse) { socket_->write(QByteArray(4, '\0')); stage_ = 3; }
            else { socket_->write(QByteArray::fromHex("00000001") + QByteArray::fromHex("00000006") + "denied"); socket_->disconnectFromHost(); stage_ = 5; }
        }
        if (stage_ == 3 && take(1, part)) {
            const QByteArray bgra = QByteArray::fromHex("20180001" "00ff00ff00ff1008" "00000000"), name("owned");
            QByteArray init = QByteArray::fromHex("00400030") + bgra; init += QByteArray::fromHex("00000005") + name;
            socket_->write(init); initialized = true; stage_ = 4;
        }
        if (stage_ == 4) buffer_.clear();
    }
    QTcpServer server_; QPointer<QTcpSocket> socket_; QByteArray buffer_; int stage_ = 0; const bool password_;
};
bool savedPasswordFixture(const QString& path, quint16 port, const QByteArray& password) {
    auto xml = ConnectionXml::create(); Secret master(QByteArray("mR3m"));
    SessionTarget target; target.protocol = "vnc"; target.host = "127.0.0.1"; target.port = port;
    const QJsonObject profile{{"id", profileId}, {"name", "Owned VNC"}, {"kind", "connection"}, {"parent", QJsonValue()}, {"overrides", target.overrides()}};
    const auto projection = QJsonDocument(QJsonObject{{"schema_version", 2}, {"profiles", QJsonArray{profile}}}).toJson(QJsonDocument::Compact);
    if (!xml || !xml->unlock(master) || !xml->patch(master, projection) || !xml->setPassword(master, profileId, Secret(password))) return false;
    if (!xml->authorizePasswords(master, profileId, {{profileId, target}})) return false;
    const auto output = xml->serialize(master);
    return output && !nodeAttribute(*output, "Password").isEmpty() && nodeAttribute(*output, "Password") != password && writeFixture(path, *output);
}
QString paneStatus(SessionPane* pane) { return pane->findChild<QLabel*>("sessionStatus")->text(); }
QString qstr(const rust::String& value) { return QString::fromUtf8(value.data(), qsizetype(value.size())); }
}

class VncSessionsTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(QDir().mkpath(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data")); }
    void cleanup() { QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 3000); }
    void vncTargetsUseLiteralPortsAndIpv6WithoutDisplayNumberGuessing() {
        struct Target { const char* input; const char* host; quint16 port; };
        for (const auto& example : {Target{"localhost", "localhost", 5900}, Target{"LOCALHOST:1", "localhost", 1},
                Target{"127.0.0.1:65535", "127.0.0.1", 65535}, Target{"[::1]", "::1", 5900},
                Target{"[::1]:5901", "::1", 5901}, Target{"::1", "::1", 5900}}) {
            const auto target = parseVncTarget(example.input); QVERIFY(target);
            QCOMPARE(target->protocol, QString("vnc")); QCOMPARE(target->host, QString(example.host)); QCOMPARE(target->port, example.port);
            QVERIFY(target->username.isEmpty()); QVERIFY(target->domain.isEmpty()); QVERIFY(!target->viewOnly);
            QCOMPARE(target->endpoint(), target->host.contains(':') ? "[" + target->host + "]:" + QString::number(example.port) : target->host + ":" + QString::number(example.port));
        }
        for (const QString input : {QString(), QString("localhost:0"), QString("localhost:65536"), QString("localhost:"),
                QString("localhost:-1"), QString("localhost:1.0"), QString("localhost::1"), QString("[::1"), QString("[::1]junk"),
                QString("[localhost]:5900"), QString("vnc://localhost"), QString("user@localhost"), QString("bad host"), QString("localhost\n")})
            QVERIFY2(!parseVncTarget(input), qPrintable(input));
    }
    void vncTargetEqualityIgnoresAccountsAndRdpOptionsButNotViewOnly() {
        auto first = *parseVncTarget("localhost:5900"); auto second = first;
        second.username = "TEAM\\inactive"; second.domain = "inactive domain"; second.options.resolution = "1920x1080";
        second.options.consoleSession = true; second.sshAuth = "private_key"; second.sshKeyPath = "/inactive/key";
        QVERIFY(first.sameAccount(second)); QVERIFY(first == second);
        auto normalized = normalizeVncTarget(second); QVERIFY(normalized);
        QVERIFY(normalized->username.isEmpty()); QVERIFY(normalized->domain.isEmpty()); QVERIFY(*normalized == first);
        const auto overrides = second.overrides(); QCOMPARE(overrides.value("protocol").toString(), QString("vnc"));
        QVERIFY(overrides.contains("view_only"));
        for (const auto* field : {"username", "domain", "resolution", "console_session", "ssh_auth", "ssh_key_path"}) QVERIFY(!overrides.contains(field));
        second.viewOnly = true; QVERIFY(first.sameAccount(second)); QVERIFY(!(first == second));
        second = first; second.port = 5901; QVERIFY(!(first == second));
        second = first; second.protocol = "ssh"; QVERIFY(!(first == second));
    }
    void savedAndTemporaryCoreRegistrationsAreGraphicalAndNeverPersistSessions() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid());
        const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path, 5900, true));
        const auto projection = QJsonDocument(storedXml(path)).toJson(QJsonDocument::Compact);
        auto core = rm::detached_core(); core->load_document(rust::Str(projection.constData(), size_t(projection.size())));
        const auto before = qstr(core->stored_document());
        const auto saved = core->open_session(profileId.toStdString(), true); QCOMPARE(core->session_count(), size_t(1));
        QCOMPARE(qstr(saved.protocol), QString("vnc")); QCOMPARE(saved.port, quint16(5900)); QVERIFY(saved.graphical); QVERIFY(saved.view_only);
        QVERIFY(saved.username.empty()); QVERIFY(saved.domain.empty()); QVERIFY(!saved.keyboard); QVERIFY(!saved.mouse); QVERIFY(!saved.clipboard); QVERIFY(!saved.resize);
        QVERIFY_THROWS_EXCEPTION(std::exception, core->mock_input(saved.id, "must not send view-only input"));
        const rm::VncTarget target{"::1", 5901, false};
        const auto temporary = core->open_temporary_vnc_session(target, true); QCOMPARE(core->session_count(), size_t(2));
        QCOMPARE(qstr(temporary.host), QString("::1")); QCOMPARE(temporary.port, quint16(5901)); QVERIFY(temporary.graphical);
        QVERIFY(!temporary.view_only); QVERIFY(temporary.keyboard); QVERIFY(temporary.mouse); QVERIFY(!temporary.clipboard); QVERIFY(!temporary.resize);
        QCOMPARE(qstr(core->stored_document()), before);
        core->close_session(saved.id); core->close_session(temporary.id); QCOMPARE(core->session_count(), size_t(0));
        QCOMPARE(qstr(core->stored_document()), before);
    }
    void quickConnectSelectorDefaultsAndMockTargetsNeverPersist() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid());
        const auto path = directory.filePath("connections.xml"); MainWindow window(path, true); window.show();
        QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open); const auto before = contents(path);
        QCOMPARE(window.quickConnectProtocol(), QString("rdp"));
        window.setQuickConnectProtocol("vnc"); QCOMPARE(window.quickConnectProtocol(), QString("vnc")); window.quickConnect("localhost", true);
        auto* pane = window.findChild<SessionPane*>(); QVERIFY(pane); QCOMPARE(pane->target().port, quint16(5900));
        QCOMPARE(pane->target().protocol, QString("vnc")); QVERIFY(pane->profile().isEmpty()); QVERIFY(!pane->document().valid());
        QVERIFY(pane->findChild<VncDesktopView*>()); QVERIFY(!pane->findChild<TerminalView*>()); QVERIFY(!pane->clipboardSharingAvailable());
        window.quickConnect("LOCALHOST:5900", true); QCOMPARE(window.sessionCount(), size_t(1));
        window.closeTab(0); QCOMPARE(window.sessionCount(), size_t(0));
        window.quickConnect("[::1]:1", true); pane = window.findChild<SessionPane*>(); QVERIFY(pane);
        QCOMPARE(pane->target().host, QString("::1")); QCOMPARE(pane->target().port, quint16(1));
        window.closeTab(0); QCOMPARE(window.sessionCount(), size_t(0)); QCOMPARE(contents(path), before);
        QVERIFY(storedXml(path).value("profiles").toArray().isEmpty());
    }
    void savedReconnectUsesOwningDocumentAndCopiedUuidsStayIndependent() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid());
        const auto a = directory.filePath("a.xml"), b = directory.filePath("b.xml"); QVERIFY(fixture(a, 5901) && fixture(b, 5902));
        MainWindow window(a, true); window.show();
        const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
        QTRY_VERIFY(window.selectConnectionProfile({idA, profileId})); window.connectSelected(true);
        auto* original = window.findChild<SessionPane*>(); QVERIFY(original); const auto session = original->id(); const auto target = original->target();
        setGridText(window, "host", "127.0.0.2"); QVERIFY(waitGridSaved(window));
        QCOMPARE(original->id(), session); QVERIFY(original->target() == target);
        QTRY_VERIFY(window.selectConnectionProfile({idB, profileId})); window.reconnectCurrent();
        original = window.findChild<SessionPane*>(); QVERIFY(original); QCOMPARE(original->document(), idA); QCOMPARE(original->profile(), profileId);
        QCOMPARE(original->target().host, QString("127.0.0.2")); QVERIFY(original->id() != session);
        window.connectSelected(true); QCOMPARE(window.sessionCount(), size_t(2));
        const auto panes = window.findChildren<SessionPane*>(); QVERIFY(panes[0]->document() != panes[1]->document());
        QCOMPARE(panes[1]->target().port, quint16(5902)); QCOMPARE(panes[1]->profile(), profileId);
        window.deleteSelected(); QVERIFY(window.lastError().contains("Close all tabs")); QCOMPARE(window.sessionCount(), size_t(2));
    }
    void nativeViewOnlyInheritanceRoundTripsAndRunningTargetsStayImmutable() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid());
        const auto body = "<Node Type=\"Container\" Id=\"" + folderId.toLatin1() + "\" Name=\"VNC group\" Protocol=\"1\" "
            "Hostname=\"127.0.0.1\" Port=\"5900\" VNCViewOnly=\"true\">" +
            "<Node Type=\"Connection\" Id=\"" + profileId.toLatin1() + "\" Name=\"child\" Protocol=\"VNC\" Hostname=\"unused.invalid\" "
            "Port=\"1\" VNCViewOnly=\"false\" InheritProtocol=\"true\" RmHostInherited=\"true\" InheritPort=\"true\" InheritVNCViewOnly=\"true\"/></Node>";
        const auto path = directory.filePath("connections.xml"); QVERIFY(writeFixture(path, nativeXml(body)));
        const auto profiles = storedXml(path).value("profiles").toArray(); QCOMPARE(profiles.size(), 2);
        const auto child = profiles[1].toObject(); QVERIFY(child.value("overrides").toObject().value("view_only").isNull());
        QVERIFY(child.value("native").toObject().value("unsupported").toArray().isEmpty());
        MainWindow window(path, true); window.show(); const auto document = window.connectionFiles().first();
        QTRY_VERIFY(window.selectConnectionProfile({document, profileId})); QVERIFY(!gridRow(window, "view_only")->isHidden());
        QVERIFY(gridValue(window, "view_only").toBool()); QVERIFY(!gridLocal(window, "view_only"));
        window.connectSelected(true); auto* pane = window.findChild<SessionPane*>(); QVERIFY(pane); QVERIFY(pane->target().viewOnly);
        QCOMPARE(pane->target().port, quint16(5900)); QCOMPARE(pane->target().host, QString("127.0.0.1"));
        const auto session = pane->id(); setGridChoice(window, "view_only", false); QVERIFY(waitGridSaved(window));
        QVERIFY(gridLocal(window, "view_only")); QVERIFY(!gridValue(window, "view_only").toBool());
        QVERIFY(pane->target().viewOnly); QCOMPARE(pane->id(), session);
        window.reconnectCurrent(); pane = window.findChild<SessionPane*>(); QVERIFY(pane); QVERIFY(!pane->target().viewOnly);
        QCOMPARE(nodeAttribute(contents(path), "VNCViewOnly"), QByteArray("false"));
        setGridLocal(window, "view_only", false); QVERIFY(gridValue(window, "view_only").toBool());
        QVERIFY(!pane->target().viewOnly); window.reconnectCurrent(); pane = window.findChild<SessionPane*>(); QVERIFY(pane->target().viewOnly);
        QCOMPARE(nodeAttribute(contents(path), "InheritVNCViewOnly"), QByteArray("true"));
    }
    void nativeIncompatibleSettingsPreventPromptsAndRegistration_data() {
        QTest::addColumn<QByteArray>("attributes"); QTest::addColumn<QString>("reason");
        QTest::newRow("mslogon") << QByteArray("VNCAuthMode=\"AuthMSLogon\"") << QString("authentication");
        QTest::newRow("proxy") << QByteArray("VNCProxyType=\"ProxyHTTP\" VNCProxyIP=\"127.0.0.1\"") << QString("gateway");
        QTest::newRow("compression") << QByteArray("VNCCompression=\"CompLevel1\"") << QString("preferences");
        QTest::newRow("encoding") << QByteArray("VNCEncoding=\"EncRRE\"") << QString("preferences");
        QTest::newRow("colors") << QByteArray("VNCColors=\"Col8Bit\"") << QString("preferences");
        QTest::newRow("malformed-view-only") << QByteArray("VNCViewOnly=\"yes\"") << QString("preferences");
        QTest::newRow("external-provider") << QByteArray("ExternalCredentialProvider=\"Vault\"") << QString("provider");
        QTest::newRow("command") << QByteArray("PreExtApp=\"never execute this\"") << QString("command");
    }
    void nativeIncompatibleSettingsPreventPromptsAndRegistration() {
        QFETCH(QByteArray, attributes); QFETCH(QString, reason);
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid());
        const auto path = directory.filePath("connections.xml"); const auto original = nativeXml(nativeNode(5900, attributes)); QVERIFY(writeFixture(path, original));
        const auto projection = QJsonDocument(storedXml(path)).toJson(QJsonDocument::Compact);
        const auto profile = QJsonDocument::fromJson(projection).object().value("profiles").toArray()[0].toObject();
        QVERIFY(profile.value("native").toObject().value("unsupported").toArray().contains(reason));
        auto core = rm::detached_core(); core->load_document(rust::Str(projection.constData(), size_t(projection.size())));
        QVERIFY_THROWS_EXCEPTION(std::exception, core->open_session(profileId.toStdString(), true)); QCOMPARE(core->session_count(), size_t(0));
        MainWindow window(path, true); window.show(); QTRY_VERIFY(window.selectConnectionProfile({window.connectionFiles().first(), profileId}));
        auto* action = window.findChild<QAction*>("actionTreeConnect"); QVERIFY(action); QVERIFY(!action->isEnabled());
        window.connectSelected(); QCOMPARE(window.sessionCount(), size_t(0)); QVERIFY(!window.findChild<VncCredentialsDialog*>());
        QCOMPARE(VncWorker::activeWorkers(), 0); QCOMPARE(contents(path), original);
    }
    void serverPasswordPromptAppearsOnlyWhenRequiredAndCancelKeepsTab() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid()); FakeRfb server(true);
        const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path, server.port())); const auto before = contents(path);
        MainWindow window(path, true); window.show(); QTRY_VERIFY(window.selectConnectionProfile({window.connectionFiles().first(), profileId}));
        // No pre-connect prompt: the session starts and the server decides.
        window.connectSelected(); QTRY_COMPARE(window.sessionCount(), size_t(1)); auto* pane = window.findChild<SessionPane*>(); QVERIFY(pane);
        QTRY_VERIFY(window.findChild<VncCredentialsDialog*>()); auto* dialog = window.findChild<VncCredentialsDialog*>();
        QCOMPARE(dialog->parentWidget(), pane); QCOMPARE(dialog->windowTitle(), QString("Password"));
        QCOMPARE(dialog->windowModality(), Qt::ApplicationModal); QVERIFY(dialog->isVisible()); QCOMPARE(server.selected, 2);
        QVERIFY(paneStatus(pane).contains("password required"));
        submitPassword(dialog, "123456789"); QVERIFY(dialog->isVisible()); QVERIFY(!dialog->findChild<QLabel*>("errorLabel")->isHidden());
        QVERIFY(server.response.isEmpty());
        dialog->reject(); QTRY_VERIFY(!window.findChild<VncCredentialsDialog*>()); QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 3000);
        QCOMPARE(window.sessionCount(), size_t(1)); QVERIFY(paneStatus(pane).contains("Disconnected")); QVERIFY(server.response.isEmpty());
        // Reconnect asks again; the correct password completes the handshake.
        window.reconnectCurrent(); QTRY_VERIFY(window.findChild<VncCredentialsDialog*>()); pane = window.findChild<SessionPane*>();
        submitPassword(window.findChild<VncCredentialsDialog*>(), "password");
        QTRY_COMPARE(server.response, PasswordResponse); QTRY_VERIFY(server.initialized);
        QTRY_VERIFY(paneStatus(pane).contains("VNC — connected")); QCOMPARE(server.accepted, 2);
        window.closeTab(0); QCOMPARE(window.sessionCount(), size_t(0)); QCOMPARE(contents(path), before);
    }
    void wrongPasswordIsReportedAndClosingDuringPromptRetires() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid()); FakeRfb server(true);
        const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path, server.port()));
        MainWindow window(path, true); window.show(); QTRY_VERIFY(window.selectConnectionProfile({window.connectionFiles().first(), profileId}));
        window.connectSelected(); QTRY_VERIFY(window.findChild<VncCredentialsDialog*>()); auto* pane = window.findChild<SessionPane*>();
        submitPassword(window.findChild<VncCredentialsDialog*>(), "wrong");
        QTRY_VERIFY(paneStatus(pane).contains("rejected the password")); QVERIFY(!server.initialized);
        window.reconnectCurrent(); QTRY_VERIFY(window.findChild<VncCredentialsDialog*>());
        QElapsedTimer timer; timer.start(); window.closeTab(0); QVERIFY(timer.elapsed() < 500);
        QVERIFY(!window.findChild<VncCredentialsDialog*>()); QCOMPARE(window.sessionCount(), size_t(0));
        QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 3000);
    }
    void savedReviewedPasswordIsSentWithoutPromptOnlyWhenRequired() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid()); FakeRfb secured(true), open(false);
        const auto path = directory.filePath("connections.xml"); QVERIFY(savedPasswordFixture(path, secured.port(), "password"));
        const Secret master(QByteArray("mR3m")); auto xml = ConnectionXml::read(contents(path), &master); QVERIFY(xml);
        const auto account = xml->committedAccount(profileId); QVERIFY(account); QCOMPARE(account->protocol, QString("vnc"));
        QVERIFY(account->username.isEmpty()); QCOMPARE(account->port, secured.port());
        MainWindow window(path, true); window.show(); const auto document = window.connectionFiles().first();
        QTRY_VERIFY(window.selectConnectionProfile({document, profileId})); QCOMPARE(gridValue(window, "password").toString(), QString(8, QChar(0x2022)));
        QVERIFY(gridRow(window, "username")->isHidden()); QVERIFY(!gridRow(window, "smart_size")->isHidden()); QVERIFY(!gridRow(window, "view_only")->isHidden());
        window.connectSelected(); QTRY_COMPARE(secured.response, PasswordResponse); QTRY_VERIFY(secured.initialized);
        auto* pane = window.findChild<SessionPane*>(); QTRY_VERIFY(paneStatus(pane).contains("VNC — connected"));
        QVERIFY(!window.findChild<VncCredentialsDialog*>()); window.closeTab(0);
        QCOMPARE(window.sessionCount(), size_t(0));
        // A reviewed saved password is never sent to a server that offers None.
        const auto openPath = directory.filePath("open.xml"); QVERIFY(savedPasswordFixture(openPath, open.port(), "password"));
        const auto openDocument = window.openConnectionFile(openPath); QTRY_VERIFY(window.selectConnectionProfile({openDocument, profileId}));
        window.connectSelected(); QTRY_VERIFY(open.initialized); QCOMPARE(open.selected, 1); QVERIFY(open.response.isEmpty());
        QVERIFY(!window.findChild<VncCredentialsDialog*>()); window.closeTab(0); QCOMPARE(window.sessionCount(), size_t(0));
    }
    void temporaryConnectsWithoutPromptWhenServerOffersNone() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid()); FakeRfb server(false);
        const auto path = directory.filePath("connections.xml"); MainWindow window(path, true); window.show();
        QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open); const auto before = contents(path);
        window.setQuickConnectProtocol("vnc");
        window.quickConnect("127.0.0.1:" + QString::number(server.port())); QTRY_VERIFY(server.initialized); QCOMPARE(server.selected, 1);
        auto* pane = window.findChild<SessionPane*>(); QVERIFY(pane && !pane->mock()); QVERIFY(pane->profile().isEmpty()); QVERIFY(!pane->document().valid());
        QTRY_VERIFY(paneStatus(pane).contains("VNC — connected")); QVERIFY(!window.findChild<VncCredentialsDialog*>());
        window.closeTab(0); QCOMPARE(window.sessionCount(), size_t(0)); QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 3000);
        QCOMPARE(contents(path), before); QVERIFY(storedXml(path).value("profiles").toArray().isEmpty());
    }
    void sessionCapturesOwningDocumentNotCurrentSelectionOrCopiedUuid() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid()); const auto port = closedLoopbackPort(); QVERIFY(port);
        const auto a = directory.filePath("a.xml"), b = directory.filePath("b.xml"); QVERIFY(fixture(a, port) && fixture(b, 5902));
        MainWindow window(a, true); window.show(); const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
        QTRY_VERIFY(window.selectConnectionProfile({idA, profileId})); window.connectSelected(); QTRY_COMPARE(window.sessionCount(), size_t(1));
        QTRY_VERIFY(window.selectConnectionProfile({idB, profileId})); setGridText(window, "name", "Different document revision"); QVERIFY(waitGridSaved(window));
        auto* pane = window.findChild<SessionPane*>(); QVERIFY(pane && !pane->mock()); QCOMPARE(pane->document(), idA);
        QCOMPARE(pane->profile(), profileId); QCOMPARE(pane->target().port, port); QVERIFY(pane->target().username.isEmpty()); QVERIFY(pane->target().domain.isEmpty());
        window.connectSelected(true); QCOMPARE(window.sessionCount(), size_t(2));
        const auto panes = window.findChildren<SessionPane*>(); QCOMPARE(panes[1]->document(), idB); QCOMPARE(panes[1]->target().port, quint16(5902));
    }
    void importedEncryptedPasswordIsPreservedAndReusedOnlyAfterReview() {
        const Secret master(QByteArray("mR3m"));
        const auto cipher = ConnectionCrypto::encrypt(master, 1000, Secret(QByteArray("imported")), 4096); QVERIFY(cipher);
        const auto original = nativeXml(nativeNode(5901, "Password=\"" + *cipher + "\" Username=\"inactive\" Domain=\"inactive\""));
        auto xml = ConnectionXml::read(original, &master); QVERIFY(xml);
        const auto account = xml->committedAccount(profileId); QVERIFY(account); QCOMPARE(account->protocol, QString("vnc"));
        QVERIFY(account->username.isEmpty()); QVERIFY(account->domain.isEmpty()); QCOMPARE(account->port, quint16(5901));
        const auto projection = xml->projection(); QVERIFY(projection); QVERIFY(!projection->contains(*cipher)); QVERIFY(!projection->contains("imported"));
        // Imported ciphertext is never sent until the account is reviewed and bound.
        QVERIFY(!xml->password(master, profileId, *account));
        QVERIFY(xml->reviewPasswords(master, {{profileId, *account}}));
        const auto password = xml->password(master, profileId, *account); QVERIFY(password);
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(password->data()), qsizetype(password->size())), QByteArray("imported"));
        auto moved = *account; moved.port = 5902; QVERIFY(!xml->password(master, profileId, moved));
        const auto saved = xml->serialize(master); QVERIFY(saved); QCOMPARE(nodeAttribute(*saved, "Password"), *cipher);
    }
    void productionPendingConnectionDisconnectReconnectAndCloseStayResponsive() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid());
        // An owned TCP socket intentionally sends no RFB greeting: no remote server or credentials are needed.
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path, server.serverPort())); const auto before = contents(path);
        MainWindow window(path, true); window.show(); QTRY_VERIFY(window.selectConnectionProfile({window.connectionFiles().first(), profileId}));
        auto* action = window.findChild<QAction*>("actionTreeConnect"); QVERIFY(action && action->isEnabled()); action->trigger();
        QTRY_COMPARE(window.sessionCount(), size_t(1)); auto* pane = window.findChild<SessionPane*>(); QVERIFY(pane && !pane->mock());
        QTRY_VERIFY(server.hasPendingConnections()); auto* socket = server.nextPendingConnection(); QVERIFY(socket);
        QSignalSpy disconnected(socket, &QTcpSocket::disconnected);
        int beats = 0; QTimer heartbeat; connect(&heartbeat, &QTimer::timeout, this, [&] { ++beats; }); heartbeat.start(1);
        QElapsedTimer elapsed; elapsed.start(); window.disconnectCurrent(); QVERIFY(elapsed.elapsed() < 100);
        QCOMPARE(window.sessionCount(), size_t(1)); QVERIFY(!pane->active());
        int retired = 0; pane->afterVncRetirement([&] { ++retired; });
        QTRY_VERIFY(!disconnected.isEmpty()); QTRY_VERIFY(beats > 0); QTRY_COMPARE(retired, 1);
        QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 3000);
        window.reconnectCurrent();
        QTRY_VERIFY(server.hasPendingConnections()); socket = server.nextPendingConnection(); QVERIFY(socket);
        QPointer<SessionPane> old = window.findChild<SessionPane*>(); QVERIFY(old);
        elapsed.restart(); window.reconnectCurrent(); QVERIFY(elapsed.elapsed() < 100);
        QTRY_VERIFY(server.hasPendingConnections()); QVERIFY(server.nextPendingConnection());
        window.closeTab(0); QCOMPARE(window.sessionCount(), size_t(0)); QTRY_VERIFY(!old);
        QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 3000);
        window.connectSelected();
        QTRY_VERIFY(server.hasPendingConnections()); QVERIFY(server.nextPendingConnection());
        elapsed.restart(); window.close(); QVERIFY(elapsed.elapsed() < 100);
        QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 3000); QTRY_VERIFY(!window.isVisible()); QCOMPARE(window.sessionCount(), size_t(0));
        QCOMPARE(contents(path), before);
    }
    void fullscreenWorksForVncAlongsideSsh() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid());
        const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path, 5900, true));
        MainWindow window(path, true); window.resize(900, 650); window.show();
        QTRY_VERIFY(window.selectConnectionProfile({window.connectionFiles().first(), profileId})); window.connectSelected(true);
        QPointer<SessionPane> vnc = window.findChild<SessionPane*>(); QVERIFY(vnc && vnc->findChild<VncDesktopView*>());
        auto* fullscreen = window.findChild<QAction*>("actionTabFullscreen"); QVERIFY(fullscreen && fullscreen->isEnabled());
        // Fullscreen applies to every session type (SSH included); VNC keeps its own pane.
        window.setQuickConnectProtocol("ssh"); window.quickConnect("localhost", true); QVERIFY(fullscreen->isEnabled());
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); QVERIFY(tabs); QCOMPARE(tabs->count(), 2);
        QPointer<SessionPane> ssh = qobject_cast<SessionPane*>(tabs->widget(1)); QVERIFY(ssh && ssh->findChild<TerminalView*>());
        QContextMenuEvent event(QContextMenuEvent::Mouse, tabs->tabBar()->tabRect(0).center(), tabs->tabBar()->mapToGlobal(tabs->tabBar()->tabRect(0).center()));
        QApplication::sendEvent(tabs->tabBar(), &event); auto* menu = window.findChild<QMenu*>("sessionContextMenu"); QVERIFY(menu);
        auto* capturedFullscreen = menu->findChild<QAction*>("actionTabFullscreen"); QVERIFY(capturedFullscreen && capturedFullscreen->isEnabled());
        menu->close(); capturedFullscreen->trigger(); QPointer<QMainWindow> full;
        QTRY_VERIFY((full = window.sessionFullscreenWindow()) && full->isFullScreen()); QVERIFY(full->isAncestorOf(vnc)); QVERIFY(!window.isFullScreen());
        QCOMPARE(vnc->target().viewOnly, true); QVERIFY(full->findChild<QWidget*>("fullscreenBar"));
        auto* close = full->findChild<QToolButton*>("closeFullscreenConnection"); QVERIFY(close); close->click();
        QTRY_VERIFY(!vnc); QTRY_VERIFY(!window.sessionFullscreenWindow()); QVERIFY(ssh && ssh->active()); QCOMPARE(window.sessionCount(), size_t(1)); QCOMPARE(tabs->count(), 1);
    }
};
QTEST_MAIN(VncSessionsTest)
#include "vnc_sessions.moc"
