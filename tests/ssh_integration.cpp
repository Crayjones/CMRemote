#include "MainWindow.h"
#include "SessionPane.h"
#include "SshCredentialsDialog.h"
#include "SshHostKeyDialog.h"
#include "SshHostKeyService.h"
#include "SshTransferDialog.h"
#include "SshWorker.h"
#include "TerminalScreen.h"
#include "OptionsDialog.h"
#include "TerminalAppearancePage.h"
#include "Views.h"
#include "ConnectionXml.h"
#include "ClipboardDriver.h"
#include "GridDriver.h"
#include "Appearance.h"
#include <QAction>
#include <QCryptographicHash>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QMenu>
#include <QTabBar>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTest>
#include <QTimer>
#include <QWindow>
#include <QMouseEvent>
#include <QScrollBar>
#include <QWheelEvent>
#include <cmath>

namespace {
const QString profileId = "e6683f1f-4e06-4200-9a17-89cd30c68b12";
QString temporaryPath() { return qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/ssh-native-XXXXXX"; }
QJsonObject oracle(const QString& path) {
    QTcpSocket socket; socket.connectToHost("127.0.0.1", quint16(qEnvironmentVariableIntValue("RM_SSH_ORACLE_PORT")));
    if (!socket.waitForConnected(3000)) return {};
    socket.write("GET " + path.toUtf8() + " HTTP/1.0\r\nHost: owned-fixture\r\n\r\n"); socket.waitForBytesWritten(3000);
    QByteArray bytes; while (socket.waitForReadyRead(3000)) bytes += socket.readAll(); bytes += socket.readAll();
    const auto end = bytes.indexOf("\r\n\r\n"); return end < 0 ? QJsonObject{} : QJsonDocument::fromJson(bytes.mid(end + 4)).object();
}
int authCount() { return oracle("/state").value("auth").toInt(-1); }
QByteArray contents(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
Qt::KeyboardModifier physicalControl() {
#ifdef Q_OS_MACOS
    return Qt::MetaModifier;
#else
    return Qt::ControlModifier;
#endif
}
void input(TerminalView* view, const QString& text) {
    view->setFocus(); QInputMethodEvent event; event.setCommitString(text); QApplication::sendEvent(view, &event);
}
void control(TerminalView* view, Qt::Key key) { QTest::keyClick(view, key, physicalControl()); }
void tmuxPrefix(TerminalView* view, Qt::Key key, Qt::KeyboardModifiers flags = Qt::NoModifier) {
    control(view, Qt::Key_B); QTest::keyClick(view, key, flags); QTest::qWait(250);
}
bool fixture(const QString& path, bool key = false) {
    auto xml = ConnectionXml::create(); Secret master(QByteArray("mR3m"));
    SessionTarget target{"127.0.0.1", quint16(qEnvironmentVariableIntValue("RM_SSH_TEST_PORT")), "sshfixture"};
    target.protocol = "ssh"; target.sshAuth = key ? "private_key" : "password";
    target.sshKeyPath = key ? qEnvironmentVariable("RM_SSH_KEY_PATH") : QString();
    const QJsonObject profile{{"id", profileId}, {"name", "Owned OpenSSH"}, {"kind", "connection"}, {"parent", QJsonValue()}, {"overrides", target.overrides()}};
    const auto projection = QJsonDocument(QJsonObject{{"schema_version", 2}, {"profiles", QJsonArray{profile}}}).toJson(QJsonDocument::Compact);
    if (!xml || !xml->unlock(master) || !xml->patch(master, projection) || !xml->setPassword(master, profileId, Secret(qgetenv("RM_SSH_TEST_PASSWORD")))) return false;
    if (!key && !xml->authorizePasswords(master, profileId, {{profileId, target}})) return false;
    const auto output = xml->serialize(master); if (!output || output->contains(qgetenv("RM_SSH_TEST_PASSWORD")) || output->contains(qgetenv("RM_SSH_TEST_PASSPHRASE"))) return false;
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(*output) == output->size();
}
bool show(MainWindow& window) {
    window.resize(1100, 850); window.show();
    // Closed dialogs leave no active window offscreen; reactivate the shell.
    if (QGuiApplication::platformName() == "offscreen") { window.activateWindow(); return QTest::qWaitForWindowActive(&window); }
    if (!QTest::qWaitForWindowExposed(&window)) return false;
    // Settle the previous native test app's unmap before requesting focus.
    QTest::qWait(100);
#ifdef Q_OS_MACOS
    activateNativeTestApplication(&window);
#endif
    window.raise(); window.activateWindow(); window.windowHandle()->requestActivate();
    const bool active = QTest::qWaitForWindowActive(&window);
    if (!active) qWarning("Native SSH window activation failed: visible=%d active=%d application=%d modal=%d", window.isVisible(), window.isActiveWindow(), int(QGuiApplication::applicationState()), QApplication::activeModalWidget() != nullptr);
    return active;
}
QString visibleText(TerminalView* view) {
    QString result; auto* screen = view->screen();
    for (int row = 0; row < screen->cells().height(); ++row) result += screen->lineText(screen->historyLines() + row) + '\n';
    return result;
}
bool ready(SessionPane* pane) {
    auto* view = pane->findChild<TerminalView*>();
    return QTest::qWaitFor([&] { return pane->findChild<QLabel*>("sessionStatus")->isHidden() && visibleText(view).contains("owned-ssh>"); }, 15000);
}
bool command(TerminalView* view, const QString& text) {
    view->setFocus(); QInputMethodEvent input; input.setCommitString(text); QApplication::sendEvent(view, &input);
    QTest::keyClick(view, Qt::Key_Return); return true;
}
SessionPane* launch(MainWindow& window, bool temporary = false, DocumentId document = {}) {
    auto* tabs = window.findChild<QTabWidget*>("sessionTabs");
    QPointer<SessionPane> previous = qobject_cast<SessionPane*>(tabs->currentWidget());
    const auto attached = [&] { auto* pane = qobject_cast<SessionPane*>(tabs->currentWidget()); return pane && pane != previous; };
    if (!QTest::qWaitFor([&] { return window.documentState() == MainWindow::DocumentState::Open; }, 15000)) return nullptr;
    if (temporary) {
        window.setQuickConnectProtocol("ssh");
        window.quickConnect("127.0.0.1:" + qEnvironmentVariable("RM_SSH_TEST_PORT"));
    } else {
        if (!document.valid()) document = window.connectionFiles().first();
        if (!QTest::qWaitFor([&] { return window.selectConnectionProfile({document, profileId}); }, 15000)) return nullptr;
        window.findChild<QAction*>("actionTreeConnect")->trigger();
    }
    if (!QTest::qWaitFor([&] { return window.findChild<SshCredentialsDialog*>() || attached(); }, 15000)) return nullptr;
    if (auto* credentials = window.findChild<SshCredentialsDialog*>()) {
        if (temporary) credentials->findChild<QLineEdit*>("usernameEdit")->setText("sshfixture");
        const bool key = credentials->findChild<QComboBox*>("authCombo")->currentData() == "private_key";
        credentials->findChild<QLineEdit*>("authenticationEdit")->setText(qEnvironmentVariable(key ? "RM_SSH_TEST_PASSPHRASE" : "RM_SSH_TEST_PASSWORD"));
        credentials->findChild<QDialogButtonBox*>("buttonBox")->button(QDialogButtonBox::Ok)->click();
    }
    if (!QTest::qWaitFor(attached, 15000)) return nullptr;
    return qobject_cast<SessionPane*>(tabs->currentWidget());
}
bool approve(SessionPane* pane) {
    // Server keys are accepted without a prompt.
    return !pane->findChild<SshHostKeyDialog*>();
}
}
class SshIntegration : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        applyLightAppearance(*qApp); qApp->setQuitOnLastWindowClosed(false);
        QVERIFY(QDir().mkpath(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data"));
        QVERIFY(!qgetenv("RM_SSH_TEST_PASSWORD").isEmpty()); QVERIFY(!qgetenv("RM_SSH_TEST_PASSPHRASE").isEmpty());
        QVERIFY(qEnvironmentVariableIntValue("RM_SSH_TEST_PORT") > 0 && authCount() >= 0);
    }
    void cleanup() {
        QTRY_COMPARE_WITH_TIMEOUT(SshWorker::activeWorkers(), 0, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(SshHostKeyService::activeWorkers(), 0, 5000);
    }
    void savedSshPasswordThroughShell() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid()); const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path));
        auto endpoint = std::make_shared<TestClipboardEndpoint>(); MainWindow window(path, true, nullptr, endpoint, directory.filePath("trust.json")); QVERIFY(show(window));
        const int before = authCount(); auto* pane = launch(window); QVERIFY(pane); QCOMPARE(window.sessionCount(), size_t(1)); QCOMPARE(authCount(), before);
        QVERIFY(!window.findChild<SshCredentialsDialog*>()); QVERIFY(approve(pane)); QVERIFY(ready(pane)); QTRY_COMPARE(authCount(), before + 1);
        command(pane->findChild<TerminalView*>(), "printf 'saved-password-ok\\n' > marker; printf 'SAVED_PASSWORD_OK\\n'");
        QTRY_VERIFY(oracle("/marker").value("text") == "saved-password-ok\n");
        QTRY_VERIFY(visibleText(pane->findChild<TerminalView*>()).contains("SAVED_PASSWORD_OK"));
        QCOMPARE(endpoint->reads, 0); QCOMPARE(endpoint->writes, 0); QCOMPARE(endpoint->watches, 0);
        window.close(); QTRY_VERIFY(!window.isVisible()); QCOMPARE(window.sessionCount(), size_t(0));
    }
    void savedSshEncryptedKeyThroughShell() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid()); const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path, true));
        MainWindow window(path, true, nullptr, std::make_shared<TestClipboardEndpoint>(), directory.filePath("trust.json")); QVERIFY(show(window));
        auto* pane = launch(window); QVERIFY(pane); QCOMPARE(pane->target().sshAuth, QString("private_key")); QVERIFY(approve(pane)); QVERIFY(ready(pane));
        command(pane->findChild<TerminalView*>(), "printf 'encrypted-key-ok\\n' > marker"); QTRY_VERIFY(oracle("/marker").value("text") == "encrypted-key-ok\n");
        window.close(); QTRY_VERIFY(!window.isVisible());
    }
    void temporarySshThroughShell() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid());
        MainWindow window(directory.filePath("connections.xml"), true, nullptr, std::make_shared<TestClipboardEndpoint>(), directory.filePath("trust.json")); QVERIFY(show(window));
        auto* pane = launch(window, true); QVERIFY(pane && pane->profile().isEmpty()); QVERIFY(approve(pane)); QVERIFY(ready(pane));
        command(pane->findChild<TerminalView*>(), "printf 'temporary-ok\\n' > marker"); QTRY_VERIFY(oracle("/marker").value("text") == "temporary-ok\n");
        window.disconnectCurrent(); QCOMPARE(window.sessionCount(), size_t(1)); window.closeTab(0); QCOMPARE(window.sessionCount(), size_t(0)); window.close();
    }
    void sshHostKeysAcceptedWithoutPromptThroughShell() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid()); const auto path = directory.filePath("connections.xml"), cache = directory.filePath("trust.json"); QVERIFY(fixture(path));
        MainWindow window(path, true, nullptr, std::make_shared<TestClipboardEndpoint>(), cache); QVERIFY(show(window));
        const int before = authCount(); auto* pane = launch(window); QVERIFY(pane); QVERIFY(ready(pane)); QVERIFY(!pane->findChild<SshHostKeyDialog*>());
        QTRY_COMPARE(authCount(), before + 1); QVERIFY(!QFile::exists(cache)); window.closeTab(0);
        // A different previously trusted key still connects without a warning or cache change.
        SshHostKeyStore store(cache); const auto old = makeSshHostKey("127.0.0.1", quint16(qEnvironmentVariableIntValue("RM_SSH_TEST_PORT")), QByteArray::fromBase64(qgetenv("RM_SSH_OLD_KEY")));
        auto inspected = store.inspect(old); QVERIFY(inspected.result.ok && inspected.inspection);
        QVERIFY(store.decide(inspected.inspection->ticket, SshHostKeyDecision::TrustAndSave).allowed);
        QFile file(cache); QVERIFY(file.open(QIODevice::ReadOnly)); const auto previous = file.readAll(); file.close();
        pane = launch(window); QVERIFY(pane); QVERIFY(ready(pane)); QVERIFY(!pane->findChild<SshHostKeyDialog*>());
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), previous); file.close();
        window.close(); QTRY_VERIFY(!window.isVisible());
    }
    void sshShellVimUnicodeAndResizeThroughShell() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid()); const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path));
        auto endpoint = std::make_shared<TestClipboardEndpoint>(); MainWindow window(path, true, nullptr, endpoint, directory.filePath("trust.json"), directory.filePath("appearance.json")); QVERIFY(show(window));
        auto* pane = launch(window); QVERIFY(pane); QVERIFY(approve(pane)); QVERIFY(ready(pane)); auto* view = pane->findChild<TerminalView*>();
        command(view, "printf '\\033[2J\\033[H' ; vim -u NONE -n /home/sshfixture/vim"); QTRY_VERIFY_WITH_TIMEOUT(view->screen()->alternate(), 15000);
        QTest::keyClick(view, Qt::Key_I); const auto text = QString::fromUtf8("owned vim é界é");
        QInputMethodEvent unicode; unicode.setCommitString(text); QApplication::sendEvent(view, &unicode);
        QTRY_VERIFY(visibleText(view).contains(text)); QTest::keyClick(view, Qt::Key_Escape); QTest::keyClicks(view, ":wq"); QTest::keyClick(view, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!view->screen()->alternate(), 15000); QTRY_VERIFY(oracle("/vim").value("text").toString() == text + '\n');
        const auto old = view->screen()->cells(); window.resize(1250, 920); QTRY_VERIFY(view->screen()->cells() != old); QTest::qWait(250);
        command(view, "stty size > /home/sshfixture/stty"); const auto cells = view->screen()->cells();
        QTRY_COMPARE(oracle("/stty").value("text").toString().trimmed(), QString("%1 %2").arg(cells.height()).arg(cells.width()));
        const auto session = pane->id(); const auto target = pane->target(); const int authenticated = authCount();
        window.findChild<QAction*>("actionTerminalAppearance")->trigger(); QTRY_VERIFY(window.findChild<OptionsDialog*>());
        auto* appearance = window.findChild<OptionsDialog*>(); appearance->findChild<QSpinBox*>("fontSize")->setValue(18);
        appearance->findChild<QDialogButtonBox*>("buttons")->button(QDialogButtonBox::Ok)->click();
        QTRY_COMPARE(view->appearance().fontSize, 18); QTRY_VERIFY(!window.findChild<OptionsDialog*>());
        QVERIFY(view->screen()->cells().width() < cells.width()); QVERIFY(view->screen()->cells().height() < cells.height());
        QCOMPARE(pane->id(), session); QVERIFY(pane->target() == target); QCOMPARE(authCount(), authenticated); QTest::qWait(250);
        command(view, "stty size > /home/sshfixture/stty"); const auto fontCells = view->screen()->cells();
        QTRY_COMPARE(oracle("/stty").value("text").toString().trimmed(), QString("%1 %2").arg(fontCells.height()).arg(fontCells.width()));
        command(view, "printf '\\033]52;c;c3ludGhldGlj\\a\\033]0;remote-title\\a' ; printf 'TERMINAL_SAFE\\n'");
        QTRY_VERIFY(visibleText(view).contains("TERMINAL_SAFE")); QCOMPARE(endpoint->reads, 0); QCOMPARE(endpoint->writes, 0); QCOMPARE(endpoint->watches, 0);
        window.close(); QTRY_VERIFY(!window.isVisible());
    }
    void sshControlShortcutsThroughShell() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid()); const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path));
        auto endpoint = std::make_shared<TestClipboardEndpoint>(); MainWindow window(path, true, nullptr, endpoint, directory.filePath("trust.json"), directory.filePath("appearance.json")); QVERIFY(show(window));
        auto* pane = launch(window); QVERIFY(pane); QVERIFY(approve(pane)); QVERIFY(ready(pane)); auto* view = pane->findChild<TerminalView*>();
        QTRY_VERIFY(!pane->findChild<SshHostKeyDialog*>()); QVERIFY(show(window)); view->setFocus(); QTRY_VERIFY(view->hasFocus());
        const auto session = pane->id(); const auto target = pane->target(); const int authenticated = authCount();
        command(view, "set -o emacs; bind 'set enable-bracketed-paste off'; : > owned-completion-target; printf 'ready\\n' > shortcuts");
        QString expected = "ready\n"; QTRY_COMPARE(oracle("/shortcuts").value("text").toString(), expected);
        input(view, "xxxxprintf 'line-edit-ok\\n' >> shortcuts"); control(view, Qt::Key_A);
        for (int i = 0; i < 4; ++i) control(view, Qt::Key_D);
        control(view, Qt::Key_E); QTest::keyClick(view, Qt::Key_Return);
        expected += "line-edit-ok\n"; QTRY_COMPARE(oracle("/shortcuts").value("text").toString(), expected);
        input(view, "printf 'kill-yank-ok\\n' >> shortcuts"); control(view, Qt::Key_A); control(view, Qt::Key_K); control(view, Qt::Key_Y); control(view, Qt::Key_E); QTest::keyClick(view, Qt::Key_Return);
        expected += "kill-yank-ok\n"; QTRY_COMPARE(oracle("/shortcuts").value("text").toString(), expected);
        input(view, "discard-this"); control(view, Qt::Key_U); command(view, "printf 'kill-before-ok\\n' >> shortcuts");
        expected += "kill-before-ok\n"; QTRY_COMPARE(oracle("/shortcuts").value("text").toString(), expected);
        input(view, "printf 'word-delete-ok\\n' >> shortcuts garbage"); control(view, Qt::Key_W); QTest::keyClick(view, Qt::Key_Return);
        expected += "word-delete-ok\n"; QTRY_COMPARE(oracle("/shortcuts").value("text").toString(), expected);
        input(view, "printf '%s\\n' wrong"); QTest::keyClick(view, Qt::Key_B, Qt::AltModifier); input(view, "other "); QTest::keyClick(view, Qt::Key_F, Qt::AltModifier);
        QTest::keyClick(view, Qt::Key_Backspace, Qt::AltModifier); control(view, Qt::Key_W); command(view, "meta-edit-ok >> shortcuts");
        expected += "meta-edit-ok\n"; QTRY_COMPARE(oracle("/shortcuts").value("text").toString(), expected);
        input(view, "printf 'completion-ok\\n' > owned-completion-t"); QTest::keyClick(view, Qt::Key_Tab); QVERIFY(view->hasFocus());
        command(view, "; cat owned-completion-target >> shortcuts"); expected += "completion-ok\n"; QTRY_COMPARE(oracle("/shortcuts").value("text").toString(), expected);
        command(view, "sleep 60; printf 'interrupt-failed\\n' >> shortcuts"); QTest::qWait(250); control(view, Qt::Key_C);
        command(view, "printf 'interrupt-ok\\n' >> shortcuts"); expected += "interrupt-ok\n"; QTRY_COMPARE(oracle("/shortcuts").value("text").toString(), expected);
        control(view, Qt::Key_L); command(view, "printf 'redraw-ok\\n' >> shortcuts"); expected += "redraw-ok\n"; QTRY_COMPARE(oracle("/shortcuts").value("text").toString(), expected);
        command(view, "tmux -L owned-shortcuts -f /dev/null new-session -s owned"); QTRY_VERIFY(view->screen()->alternate()); QTest::qWait(500);
        const QString state = "tmux -L owned-shortcuts display-message -p '#{session_windows} #{window_index} #{window_panes}' >> /home/sshfixture/shortcuts";
        command(view, state); expected += "1 0 1\n"; QTRY_COMPARE(oracle("/shortcuts").value("text").toString(), expected);
        tmuxPrefix(view, Qt::Key_C); command(view, state); expected += "2 1 1\n"; QTRY_COMPARE(oracle("/shortcuts").value("text").toString(), expected);
        tmuxPrefix(view, Qt::Key_P); command(view, state); expected += "2 0 1\n"; QTRY_COMPARE(oracle("/shortcuts").value("text").toString(), expected);
        tmuxPrefix(view, Qt::Key_N); command(view, state); expected += "2 1 1\n"; QTRY_COMPARE(oracle("/shortcuts").value("text").toString(), expected);
        tmuxPrefix(view, Qt::Key_Percent, Qt::ShiftModifier); command(view, state); expected += "2 1 2\n"; QTRY_COMPARE(oracle("/shortcuts").value("text").toString(), expected);
        tmuxPrefix(view, Qt::Key_D); QTRY_VERIFY(!view->screen()->alternate()); QVERIFY(ready(pane));
        command(view, "tmux -L owned-shortcuts kill-server; printf 'detached-ok\\n' >> shortcuts"); expected += "detached-ok\n"; QTRY_COMPARE(oracle("/shortcuts").value("text").toString(), expected);
        QCOMPARE(pane->id(), session); QVERIFY(pane->target() == target); QCOMPARE(authCount(), authenticated); QCOMPARE(window.sessionCount(), size_t(1));
        QCOMPARE(endpoint->reads, 0); QCOMPARE(endpoint->writes, 0); QCOMPARE(endpoint->watches, 0); window.close(); QTRY_VERIFY(!window.isVisible());
    }
    void sshIndexedColorsThroughShell() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid());
        const auto path = directory.filePath("connections.xml"), trust = directory.filePath("trust.json"), settings = directory.filePath("appearance.json");
        QVERIFY(fixture(path)); auto endpoint = std::make_shared<TestClipboardEndpoint>();
        MainWindow window(path, true, nullptr, endpoint, trust, settings); QVERIFY(show(window));
        auto* pane = launch(window); QVERIFY(pane); QVERIFY(approve(pane)); QVERIFY(ready(pane)); auto* view = pane->findChild<TerminalView*>();
        QTRY_VERIFY(!pane->findChild<SshHostKeyDialog*>()); QVERIFY(show(window)); view->setFocus(); QTRY_VERIFY(view->hasFocus());
        const auto session = pane->id(); const auto target = pane->target(); const int authenticated = authCount();
        const auto xmlBytes = contents(path), trustBytes = contents(trust), settingsBytes = contents(settings); auto* screen = view->screen();
        const QString samples = "printf '\\033[2J\\033[H\\033[48;5;28mBGI\\033[0m\\r\\n"
            "\\033[48;2;0;135;0mBGT\\033[0m\\r\\n\\033[38;5;28mFGI\\033[0m\\r\\n"
            "\\033[38;2;0;135;0mFGT\\033[0m\\r\\n'; ";
        const auto samplesPresent = [&] {
            return screen->lineText(screen->historyLines()) == "BGI" && screen->lineText(screen->historyLines() + 1) == "BGT" &&
                screen->lineText(screen->historyLines() + 2) == "FGI" && screen->lineText(screen->historyLines() + 3) == "FGT";
        };
        const auto verifyColors = [&] {
            for (int column = 0; column < 3; ++column) {
                QCOMPARE(screen->cell(0, column).background, QColor("#008700"));
                QCOMPARE(screen->cell(1, column).background, screen->cell(0, column).background);
                QCOMPARE(screen->cell(2, column).foreground, QColor("#008700"));
                QCOMPARE(screen->cell(3, column).foreground, screen->cell(2, column).foreground);
            }
        };
        command(view, samples + "printf 'outside:%s\\n' \"$TERM\" > marker");
        QTRY_COMPARE(oracle("/marker").value("text").toString(), QString("outside:xterm-256color\n"));
        QTRY_VERIFY(samplesPresent()); verifyColors(); QVERIFY(!screen->alternate());
        command(view, "tmux -L owned-colors -f /dev/null new-session -s owned"); QTRY_VERIFY(screen->alternate()); QTest::qWait(500);
        command(view, samples + "printf 'inside:%s\\n' \"$TERM\" >> /home/sshfixture/marker");
        QTRY_VERIFY(oracle("/marker").value("text").toString().startsWith("outside:xterm-256color\ninside:"));
        const auto terms = oracle("/marker").value("text").toString().split('\n'); QCOMPARE(terms.size(), 3); QVERIFY(terms[1].size() > 7);
        qInfo("Owned color fixture TERM: %s; %s", qPrintable(terms[0]), qPrintable(terms[1]));
        QTRY_VERIFY(samplesPresent()); verifyColors();
        tmuxPrefix(view, Qt::Key_D); QTRY_VERIFY(!screen->alternate()); QVERIFY(ready(pane));
        command(view, "tmux -L owned-colors kill-server; printf 'detached-ok\\n' >> marker");
        QTRY_COMPARE(oracle("/marker").value("text").toString(), terms.join('\n') + "detached-ok\n");
        QCOMPARE(pane->id(), session); QVERIFY(pane->target() == target); QCOMPARE(authCount(), authenticated); QCOMPARE(window.sessionCount(), size_t(1));
        QCOMPARE(contents(path), xmlBytes); QCOMPARE(contents(trust), trustBytes); QCOMPARE(contents(settings), settingsBytes);
        QCOMPARE(endpoint->reads, 0); QCOMPARE(endpoint->writes, 0); QCOMPARE(endpoint->watches, 0);
        window.close(); QTRY_VERIFY(!window.isVisible());
    }
    void sshScrollbackDragSelectionThroughShell() {
        QTemporaryDir directory(temporaryPath()); const auto path = directory.filePath("connections.xml"), trust = directory.filePath("trust.json"), settings = directory.filePath("appearance.json");
        QVERIFY(fixture(path)); auto endpoint = std::make_shared<TestClipboardEndpoint>(); MainWindow window(path, true, nullptr, endpoint, trust, settings); QVERIFY(show(window));
        auto* pane = launch(window); QVERIFY(pane); QVERIFY(approve(pane)); QVERIFY(ready(pane)); auto* view = pane->findChild<TerminalView*>();
        auto* other = launch(window, true); QVERIFY(other); QVERIFY(approve(other)); QVERIFY(ready(other)); auto* otherView = other->findChild<TerminalView*>();
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); tabs->setCurrentWidget(pane); QVERIFY(show(window)); view->setFocus(); QTRY_VERIFY(view->hasFocus());
        const auto session = pane->id(), otherSession = other->id(); const auto target = pane->target(); const int authenticated = authCount();
        const auto xmlBytes = contents(path), trustBytes = contents(trust), settingsBytes = contents(settings);
        const auto otherCells = otherView->screen()->cells(); const auto otherText = visibleText(otherView);
        command(view, "printf '\\033[2J\\033[H'; for i in $(seq 0 159); do printf 'owned-scroll-%04d\\n' \"$i\"; done; printf 'scroll-ready\\n' > marker");
        QTRY_COMPARE(oracle("/marker").value("text").toString(), QString("scroll-ready\n"));
        const auto line = [](int value) { return QString("owned-scroll-%1").arg(value, 4, 10, QLatin1Char('0')); };
        QTRY_VERIFY(visibleText(view).contains(line(159))); auto* screen = view->screen(); auto* scroll = view->findChild<QScrollBar*>();
        QCOMPARE(screen->lineText(0), line(0)); QVERIFY(scroll->maximum() > 65);
        const QFontMetricsF metrics(view->font()); const int width = int(std::ceil(metrics.horizontalAdvance('M'))), height = int(std::ceil(metrics.height()));
        const auto position = [=](int column, int row) { return QPoint(column * width + width / 2, row * height + height / 2); };
        const auto move = [&](QPoint point) {
            QMouseEvent event(QEvent::MouseMove, point, view->mapToGlobal(point), Qt::NoButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(view, &event);
        };
        const auto wheel = [&](QPoint pixel, QPoint angle) {
            const auto point = position(0, 2); QWheelEvent event(point, view->mapToGlobal(point), pixel, angle, Qt::LeftButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(view, &event);
        };
        const auto expected = [&](int first, int last) {
            QStringList lines; for (int value = first; value <= last; ++value) lines += line(value); return lines.join('\n');
        };
#ifdef Q_OS_MACOS
        const auto copy = Qt::KeyboardModifiers(Qt::ControlModifier);
#else
        const auto copy = Qt::ControlModifier | Qt::ShiftModifier;
#endif
        scroll->setValue(62); QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, position(line(65).size() - 1, 3)); move(position(0, 2));
        wheel({}, QPoint(0, 120)); QCOMPARE(scroll->value(), 59); QCOMPARE(endpoint->writes, 0); QCOMPARE(endpoint->reads, 0); QCOMPARE(endpoint->watches, 0);
        QTest::keyClick(view, Qt::Key_C, copy); QCOMPARE(endpoint->text.value(), expected(61, 65));
        wheel(QPoint(0, height), {}); QCOMPARE(scroll->value(), 58); QTest::keyClick(view, Qt::Key_C, copy); QCOMPARE(endpoint->text.value(), expected(60, 65));
        move(QPoint(1, -3 * height)); QTRY_COMPARE_WITH_TIMEOUT(scroll->value(), 0, 5000);
        QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, QPoint(1, -3 * height)); QTest::keyClick(view, Qt::Key_C, copy);
        QCOMPARE(endpoint->text.value(), expected(0, 65)); auto* timer = view->findChild<QTimer*>("terminalSelectionScroll"); QVERIFY(timer); QVERIFY(!timer->isActive());
        scroll->setValue(10); QTest::keyClick(view, Qt::Key_C, copy); QCOMPARE(endpoint->text.value(), expected(0, 65));
        QCOMPARE(endpoint->writes, 4); QCOMPARE(endpoint->reads, 0); QCOMPARE(endpoint->watches, 0);
        QCOMPARE(pane->id(), session); QVERIFY(pane->target() == target); QCOMPARE(other->id(), otherSession); QCOMPARE(authCount(), authenticated);
        QCOMPARE(otherView->screen()->cells(), otherCells); QCOMPARE(visibleText(otherView), otherText); QCOMPARE(window.sessionCount(), size_t(2));
        QCOMPARE(contents(path), xmlBytes); QCOMPARE(contents(trust), trustBytes); QCOMPARE(contents(settings), settingsBytes);
        window.closeTab(1); window.closeTab(0); QTRY_COMPARE(window.sessionCount(), size_t(0));
    }
    void sshSessionZoomThroughShell() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid()); const auto path = directory.filePath("connections.xml"), settings = directory.filePath("appearance.json"), trust = directory.filePath("trust.json"); QVERIFY(fixture(path));
        auto endpoint = std::make_shared<TestClipboardEndpoint>(); MainWindow window(path, true, nullptr, endpoint, trust, settings); QVERIFY(show(window));
        auto* pane = launch(window); QVERIFY(pane); QVERIFY(approve(pane)); QVERIFY(ready(pane)); auto* view = pane->findChild<TerminalView*>();
        auto* other = launch(window, true); QVERIFY(other); QVERIFY(ready(other)); auto* otherView = other->findChild<TerminalView*>();
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); tabs->setCurrentWidget(pane); QVERIFY(show(window)); view->setFocus(); QTRY_VERIFY(view->hasFocus());
        const auto session = pane->id(); const auto target = pane->target(); const int authenticated = authCount();
        const auto xmlBytes = contents(path), settingBytes = contents(settings), trustBytes = contents(trust); const auto cells = view->screen()->cells(), otherCells = otherView->screen()->cells();
        QTest::keyClick(view, Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier); QCOMPARE(view->appearance().fontSize, 13); QVERIFY(view->screen()->cells() != cells);
        QCOMPARE(otherView->appearance().fontSize, 12); QCOMPARE(otherView->screen()->cells(), otherCells); QTest::qWait(250);
        command(view, "stty size > /home/sshfixture/stty"); const auto zoomed = view->screen()->cells();
        QTRY_COMPARE(oracle("/stty").value("text").toString().trimmed(), QString("%1 %2").arg(zoomed.height()).arg(zoomed.width()));
        QTest::keyClick(view, Qt::Key_Minus, Qt::ControlModifier); QCOMPARE(view->appearance().fontSize, 12); QTest::qWait(250);
        command(view, "stty size > /home/sshfixture/stty"); QTRY_COMPARE(oracle("/stty").value("text").toString().trimmed(), QString("%1 %2").arg(cells.height()).arg(cells.width()));
        QTest::keyClick(view, Qt::Key_Plus, Qt::ControlModifier); QCOMPARE(contents(path), xmlBytes); QCOMPARE(contents(settings), settingBytes); QCOMPARE(contents(trust), trustBytes);
        QCOMPARE(pane->id(), session); QVERIFY(pane->target() == target); QCOMPARE(authCount(), authenticated);
        window.findChild<QAction*>("actionTerminalAppearance")->trigger(); QTRY_VERIFY(window.findChild<OptionsDialog*>());
        auto* dialog = window.findChild<OptionsDialog*>(); dialog->findChild<QSpinBox*>("fontSize")->setValue(16);
        dialog->findChild<QDialogButtonBox*>("buttons")->button(QDialogButtonBox::Ok)->click(); QTRY_COMPARE(view->appearance().fontSize, 17); QCOMPARE(otherView->appearance().fontSize, 16);
        QTRY_VERIFY(!window.findChild<OptionsDialog*>()); const auto applied = contents(settings);
        const auto point = tabs->tabBar()->tabRect(tabs->indexOf(pane)).center();
        QContextMenuEvent context(QContextMenuEvent::Mouse, point, tabs->tabBar()->mapToGlobal(point)); QApplication::sendEvent(tabs->tabBar(), &context);
        auto* menu = window.findChild<QMenu*>("sessionContextMenu"); QVERIFY(menu); auto* different = menu->findChild<QAction*>("actionDifferentCredentials"); QVERIFY(different && different->isEnabled()); different->trigger(); menu->close();
        QTRY_VERIFY(window.findChild<SshCredentialsDialog*>()); window.findChild<SshCredentialsDialog*>()->reject(); QTRY_VERIFY(!window.findChild<SshCredentialsDialog*>());
        QCOMPARE(view->appearance().fontSize, 17); QCOMPARE(pane->id(), session); QVERIFY(pane->target() == target); QCOMPARE(authCount(), authenticated);
        window.reconnectCurrent(); QTRY_VERIFY(qobject_cast<SessionPane*>(tabs->currentWidget())->id() != session);
        auto* reconnect = qobject_cast<SessionPane*>(tabs->currentWidget()); QVERIFY(ready(reconnect)); QCOMPARE(reconnect->findChild<TerminalView*>()->appearance().fontSize, 16);
        QVERIFY(reconnect->target() == target); QTRY_COMPARE(authCount(), authenticated + 1);
        QCOMPARE(contents(path), xmlBytes); QCOMPARE(contents(settings), applied); QCOMPARE(contents(trust), trustBytes);
        QCOMPARE(endpoint->reads, 0); QCOMPARE(endpoint->writes, 0); QCOMPARE(endpoint->watches, 0); window.close(); QTRY_VERIFY(!window.isVisible());
    }
    void sshIndependentSessionsAndCancellationThroughShell() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid()); const auto path = directory.filePath("a.xml"), copy = directory.filePath("b.xml"); QVERIFY(fixture(path) && QFile::copy(path, copy));
        MainWindow window(path, true, nullptr, std::make_shared<TestClipboardEndpoint>(), directory.filePath("trust.json")); QVERIFY(show(window));
        auto* first = launch(window); QVERIFY(first); QVERIFY(approve(first)); QVERIFY(ready(first)); const auto firstId = first->id();
        const auto secondDocument = window.openConnectionFile(copy); QTRY_VERIFY(window.selectConnectionProfile({secondDocument, profileId}));
        auto* second = launch(window, false, secondDocument); QVERIFY(second && second != first); QVERIFY(approve(second)); QVERIFY(ready(second));
        QCOMPARE(window.sessionCount(), size_t(2)); QVERIFY(first->document() != second->document());
        command(first->findChild<TerminalView*>(), "printf 'first-owned\\n' > first"); command(second->findChild<TerminalView*>(), "printf 'second-owned\\n' > second");
        QTRY_VERIFY(oracle("/first").value("text") == "first-owned\n"); QTRY_VERIFY(oracle("/second").value("text") == "second-owned\n");
        window.disconnectCurrent(); QCOMPARE(window.sessionCount(), size_t(2)); QCOMPARE(first->id(), firstId);
        int beats = 0; QTimer timer; connect(&timer, &QTimer::timeout, this, [&] { ++beats; }); timer.start(1);
        QElapsedTimer elapsed; elapsed.start(); window.close(); QVERIFY(elapsed.elapsed() < 100);
        QTRY_VERIFY(!window.isVisible()); QTRY_VERIFY(beats > 0); QCOMPARE(window.sessionCount(), size_t(0));
    }
    void sshScpUploadThroughTransferDialog() {
        QTemporaryDir dir(temporaryPath()); QVERIFY(dir.isValid());
        QFile local(dir.filePath("upload test.txt")); QVERIFY(local.open(QIODevice::WriteOnly));
        QByteArray content; for (int i = 0; i < 20000; ++i) content += "line " + QByteArray::number(i) + " éß\n";
        local.write(content); local.close();
        SshHostKeyService trust(dir.filePath("ssh-host-keys.json"));
        SessionTarget target{"127.0.0.1", quint16(qEnvironmentVariableIntValue("RM_SSH_TEST_PORT")), "sshfixture"};
        target.protocol = "ssh"; target.sshAuth = "password";
        SshTransferDialog dialog(target, &trust); dialog.show();
        dialog.setPassword(Secret(qgetenv("RM_SSH_TEST_PASSWORD"))); dialog.setLocalFile(local.fileName()); dialog.setRemotePath("~/");
        bool done = false, ok = false; QString message;
        connect(&dialog, &SshTransferDialog::finished, &dialog, [&](bool result, QString text) { done = true; ok = result; message = text; });
        dialog.start();
        QTRY_VERIFY_WITH_TIMEOUT(dialog.findChild<SshHostKeyDialog*>() || done, 30000);
        if (auto* prompt = dialog.findChild<SshHostKeyDialog*>()) prompt->findChild<QPushButton*>("sshTrustSave")->click();
        QTRY_VERIFY_WITH_TIMEOUT(done, 60000); QVERIFY2(ok, qPrintable(message));
        // Compare the server's SHA-256 of the uploaded file, read over the same exec channel.
        QByteArray received; bool connected = false, ended = false;
        auto reader = std::make_unique<SshWorker>(target, rm::ssh::password_auth([] { rust::Vec<uint8_t> v; for (char c : qgetenv("RM_SSH_TEST_PASSWORD")) v.push_back(uint8_t(c)); return v; }()),
                                                  quint64(990001), nullptr, "sha256sum ~/'upload test.txt'");
        connect(reader.get(), &SshWorker::output, reader.get(), [&](QByteArray bytes) { received += bytes; });
        connect(reader.get(), &SshWorker::hostKey, reader.get(), [&](QString request, SshHostKey, quint64 generation) { reader->answerHost(request, generation, true); });
        connect(reader.get(), &SshWorker::phaseChanged, reader.get(), [&](SshWorker::Phase phase) {
            if (phase == SshWorker::Phase::Connected) connected = true;
            if (phase == SshWorker::Phase::Disconnected || phase == SshWorker::Phase::Failed) ended = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(ended, 60000); Q_UNUSED(connected);
        const auto expected = QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex();
        QTRY_VERIFY_WITH_TIMEOUT(received.startsWith(expected), 10000);
        reader->stop(); QTRY_VERIFY_WITH_TIMEOUT(SshWorker::activeWorkers() == 0, 30000);
        qInfo("RM_ACCEPTED scp upload %lld bytes", qlonglong(content.size()));
    }
};
QTEST_MAIN(SshIntegration)
#include "ssh_integration.moc"
