#include "RdpWorker.h"
#include "RdpSecurity.h"
#include "Views.h"
#include "MainWindow.h"
#include "PanelWindow.h"
#include "SessionPane.h"
#include "Appearance.h"
#include <QDockWidget>
#include "DocumentService.h"
#include "ConnectionXml.h"
#include "XmlTestHelpers.h"
#include "PropertyDelegate.h"
#include "ProfileEditor.h"
#include "GridDriver.h"
#include "ConnectionTree.h"
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDialogButtonBox>
#include <QDialog>
#include <QCheckBox>
#include <QLineEdit>
#include <QComboBox>
#include <QToolButton>
#include <QTreeWidget>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>
#include <QAbstractButton>
#include <QTabWidget>
#include <QTabBar>
#include <QMenu>
#include <QAction>
#include <QContextMenuEvent>
#include <QTemporaryDir>
#include <QFile>
#include <algorithm>
#include <QDir>
#include <QSignalSpy>
#include <QPointer>
#include <QTest>

#define WAIT_STORE(window) do { QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open); QVERIFY(waitGridSaved(window)); } while (false)

static QList<PanelWindow*> panelWindows() {
    QList<PanelWindow*> result;
    for (auto* widget : QApplication::topLevelWidgets()) if (auto* window = qobject_cast<PanelWindow*>(widget); window && window->isVisible()) result << window;
    return result;
}
static void dragTab(QTabBar* bar, int index, const QList<QPoint>& path) {
    QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(index).center());
    for (const auto& global : path) {
        QMouseEvent move(QEvent::MouseMove, QPointF(bar->mapFromGlobal(global)), QPointF(global), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(bar, &move);
    }
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(bar->mapFromGlobal(path.last())), QPointF(path.last()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(bar, &release);
}
class RdpIntegration : public QObject {
    Q_OBJECT
    void credentials(MainWindow& window, const QByteArray& password) {
        QTRY_VERIFY(window.findChild<QDialog*>("CredentialsDialog")); auto* dialog = window.findChild<QDialog*>("CredentialsDialog");
        QCOMPARE(dialog->findChild<QLineEdit*>("passwordEdit")->echoMode(), QLineEdit::Password);
        QVERIFY(!dialog->findChild<QCheckBox*>("clipboardCheck")->isChecked());
        auto* username = dialog->findChild<QLineEdit*>("usernameEdit");
        if (username->isEnabled()) username->setText("rdpfixture");
        dialog->findChild<QLineEdit*>("passwordEdit")->setText(QString::fromUtf8(password)); dialog->accept();
        QVERIFY(dialog->findChild<QLineEdit*>("passwordEdit")->text().isEmpty());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void certificate(SessionPane* pane) {
        // Server certificates are accepted without a prompt.
        QTRY_VERIFY_WITH_TIMEOUT(!pane->findChild<DesktopView*>()->frame().isNull(), 30000);
        QVERIFY(!pane->findChild<QMessageBox*>("rdpCertificate"));
    }
    QByteArray contents(const QString& path) {
        QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll();
    }
    bool fixtureReady(DesktopView* view) {
        if (view->frame().isNull()) return false;
        const auto pixel = view->frame().pixelColor(0, 0);
        // RGB565 and lossy GFX paths quantize the animated fixture's background.
        for (const QColor expected : {QColor("#205050"), QColor("#405020")})
            if (qAbs(pixel.red() - expected.red()) <= 8 && qAbs(pixel.green() - expected.green()) <= 4 && qAbs(pixel.blue() - expected.blue()) <= 8) return true;
        return false;
    }
    void presentedFixture(DesktopView* view) {
        if (QGuiApplication::platformName() == "offscreen") return; // CPU/transport validation only.
        QSignalSpy submitted(view, &DesktopView::framePresented), failed(view, &DesktopView::renderFailed);
        const auto before = view->frameRevision();
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(submitted.cbegin(), submitted.cend(), [before](const auto& entry) {
            return entry.at(0).toULongLong() > before;
        }), 10000);
        QVERIFY(failed.isEmpty()); QVERIFY(view->rendererAvailable());
        // Only grab after a newer remote frame was painted and submitted by the window.
        const auto shot = view->grabFramebuffer(); QVERIFY(!shot.isNull());
        const auto scaled = view->frame().size().scaled(view->size(), Qt::KeepAspectRatio);
        const QRect area(QPoint((view->width() - scaled.width()) / 2, (view->height() - scaled.height()) / 2), scaled);
        const qreal sx = qreal(shot.width()) / view->width(), sy = qreal(shot.height()) / view->height();
        const auto pixel = shot.pixelColor(int((area.left() + 3) * sx), int((area.top() + 3) * sy));
        bool background = false;
        for (const QColor expected : {QColor("#205050"), QColor("#405020")})
            background |= qAbs(pixel.red() - expected.red()) <= 8 && qAbs(pixel.green() - expected.green()) <= 4 && qAbs(pixel.blue() - expected.blue()) <= 8;
        QVERIFY2(background, "Submitted desktop pixels do not match the owned fixture");
        if (area.top() > 0) QCOMPARE(shot.pixelColor(int(area.center().x() * sx), int(area.top() / 2 * sy)), QColor(Qt::black));
        if (area.left() > 0) QCOMPARE(shot.pixelColor(int(area.left() / 2 * sx), int(area.center().y() * sy)), QColor(Qt::black));
    }
    void shellInput(SessionPane* pane, Qt::Key key = Qt::Key_A) {
        auto* view = pane->findChild<DesktopView*>(); QVERIFY(view);
        auto* status = pane->findChild<QLabel*>("sessionStatus"); QVERIFY(status);
        QTRY_VERIFY(status->isHidden()); QTRY_COMPARE(view->geometry(), pane->contentsRect());
        // Initial Xorg frames can precede the fixture's input handlers and animated window.
        QTRY_VERIFY_WITH_TIMEOUT(fixtureReady(view), 15000);
        presentedFixture(view);
        view->setFocus();
        QSignalSpy keys(view, &DesktopView::keyInput), pointers(view, &DesktopView::pointerInput);
        QSignalSpy frames(pane->findChild<RdpWorker*>(), &RdpWorker::frameAvailable);
        const auto scaled = view->frame().size().scaled(view->size(), Qt::KeepAspectRatio);
        const QRect image(QPoint((view->width() - scaled.width()) / 2, (view->height() - scaled.height()) / 2), scaled);
        const int divisor = key == Qt::Key_B ? 4 : 2;
        const auto point = image.topLeft() + QPoint(qRound(double(image.width()) / divisor), qRound(double(image.height()) / divisor));
        const QPoint expected(view->frame().width() / divisor, view->frame().height() / divisor);
        QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier, point); QTest::keyClick(view, key);
        QVERIFY(keys.size() >= 2); QVERIFY(pointers.size() >= 2);
        QPoint delivered(-1, -1);
        for (const auto& input : pointers)
            if ((input.at(0).toUInt() & 0x9000) == 0x9000) delivered = QPoint(input.at(1).toUInt(), input.at(2).toUInt());
        QVERIFY2(qAbs(delivered.x() - expected.x()) <= 3 && qAbs(delivered.y() - expected.y()) <= 3,
                 qPrintable(QString("Mapped press %1,%2; expected %3,%4").arg(delivered.x()).arg(delivered.y()).arg(expected.x()).arg(expected.y())));
        // Let queued protocol input run before an immediate Disconnect can cancel the worker.
        QTRY_VERIFY_WITH_TIMEOUT(frames.size() > 2, 10000);
    }
    bool showXmlWindow(MainWindow& window) {
        window.show();
        if (QGuiApplication::platformName() == "offscreen") return true;
        if (!QTest::qWaitForWindowExposed(&window)) return false;
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() == "cocoa") activateNativeTestApplication(&window);
#endif
        window.raise(); window.activateWindow(); return QTest::qWaitForWindowActive(&window);
    }
    bool xmlReady(MainWindow& window, DocumentId id) {
        auto* files = window.findChild<DocumentService*>(); auto* editor = window.findChild<ProfileEditor*>();
        return files && editor && QTest::qWaitFor([&] { return files->status(id) && !files->status(id)->busy && !editor->dirty(); }, 15000);
    }
    bool acceptXmlPasswordReview(MainWindow& window) {
        return window.findChild<QMessageBox*>("passwordReuseReview") == nullptr;
    }
    bool enterXmlMaster(MainWindow& window, const QByteArray& master, bool confirmed) {
        if (!QTest::qWaitFor([&] { return window.findChild<QDialog*>("filePassword") != nullptr; })) return false;
        auto* dialog = window.findChild<QDialog*>("filePassword"); auto* input = dialog->findChild<QLineEdit*>("masterEdit");
        if (!input || input->echoMode() != QLineEdit::Password) return false;
        input->setFocus(); QTest::keyClicks(input, QString::fromLatin1(master));
        if (confirmed) {
            auto* confirmation = dialog->findChild<QLineEdit*>("masterConfirmEdit");
            if (!confirmation || confirmation->echoMode() != QLineEdit::Password) return false;
            confirmation->setFocus(); QTest::keyClicks(confirmation, QString::fromLatin1(master));
        }
        auto* buttons = dialog->findChild<QDialogButtonBox*>(); if (!buttons || !buttons->button(QDialogButtonBox::Ok)->isEnabled()) return false;
        QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
        const bool cleared = input->text().isEmpty(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return cleared && !window.findChild<QDialog*>("filePassword");
    }
    bool unlockXml(MainWindow& window, DocumentId id, const QByteArray& master) {
        auto* files = window.findChild<DocumentService*>();
        if (!files || !QTest::qWaitFor([&] { return files->status(id) && !files->status(id)->busy; }, 15000) || !window.selectConnectionFile(id)) return false;
        if (files->status(id)->unlocked) return true;
        window.findChild<QAction*>("actionUnlockPasswords")->trigger();
        return enterXmlMaster(window, master, false) && xmlReady(window, id) && files->status(id)->unlocked;
    }
    bool confirmXmlProtection(MainWindow& window) {
        if (!QTest::qWaitFor([&] { return window.findChild<QMessageBox*>("confirmProtection") != nullptr; })) return false;
        auto* question = window.findChild<QMessageBox*>("confirmProtection");
        if (question->defaultButton() != question->button(QMessageBox::Cancel)) return false;
        QTest::mouseClick(question->button(QMessageBox::Yes), Qt::LeftButton);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return !window.findChild<QDialog*>("filePassword");
    }
    SessionPane* connectXml(MainWindow& window, ProfileReference reference, int count) {
        window.connectSelected(); auto* tabs = window.findChild<QTabWidget*>("sessionTabs");
        if (!QTest::qWaitFor([&] { return tabs->count() == count || window.findChild<QDialog*>("CredentialsDialog"); }, 15000) ||
            window.findChild<QDialog*>("CredentialsDialog")) return nullptr;
        auto* pane = qobject_cast<SessionPane*>(tabs->currentWidget());
        return pane && pane->reference() == reference && !pane->mock() ? pane : nullptr;
    }
    bool captureXmlWindow(MainWindow& window, const QString& filename) {
        if (window.findChild<PropertyDelegate*>()->activeEditor()) return false;
        for (auto* dialog : window.findChildren<QDialog*>()) if (dialog->isVisible()) return false;
        window.findChild<QAction*>("actionResetLayout")->trigger();
        auto* tree = window.findChild<QTreeWidget*>("connectionTree");
        auto* config = window.findChild<QTreeWidget*>("configTree");
        if (!QTest::qWaitFor([&] { return tree->isVisible() && config->isVisible(); })) return false;
        QTest::qWait(150);
        return window.grab().save(qEnvironmentVariable("RM_BUILD_DIR") + filename);
    }
    void assertXmlSecretFree(MainWindow& window, const QByteArray& password, const QByteArray& master) {
        const auto draft = QJsonDocument(window.findChild<ProfileEditor*>()->draft()).toJson();
        QVERIFY(!draft.contains(password) && !draft.contains(master));
        for (const auto* name : {"configTree", "connectionTree"}) {
            auto* tree = window.findChild<QTreeWidget*>(name); QVERIFY(tree);
            for (QTreeWidgetItemIterator it(tree); *it; ++it) for (int column = 0; column < tree->columnCount(); ++column)
                for (int role = Qt::DisplayRole; role <= PropertyDelegate::CategoryRole; ++role) {
                    const auto value = (*it)->data(column, role).toString().toUtf8(); QVERIFY(!value.contains(password) && !value.contains(master));
                }
        }
    }
private slots:
    void initTestCase() { applyLightAppearance(*qApp); qApp->setQuitOnLastWindowClosed(false); }
    void init() { if (QGuiApplication::platformName() != "offscreen") QTest::failOnWarning("QRhiWidget: No QRhi"); }
    void configPasswordXmlThroughShell_data() {
        QTest::addColumn<bool>("full"); QTest::addColumn<bool>("custom");
        QTest::newRow("default-password-only") << false << false; QTest::newRow("default-full-inner") << true << false;
        QTest::newRow("custom-password-only") << false << true; QTest::newRow("custom-full-inner") << true << true;
    }
    void configPasswordXmlThroughShell() {
        QFETCH(bool, full); QFETCH(bool, custom);
        const auto password = qgetenv("RM_RDP_TEST_PASSWORD"), master = qgetenv("RM_XML_TEST_MASTER");
        const int port = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT"); QVERIFY(!password.isEmpty() && master.size() >= 12 && port > 0);
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/xml-password-XXXXXX"); QVERIFY(dir.isValid());
        const auto path = dir.filePath("confCons.xml"), copy = dir.filePath("portable.xml"); QString profile; QByteArray saved;
        {
            MainWindow window(path); QVERIFY(showXmlWindow(window)); const auto id = window.connectionFiles().first(); QVERIFY(xmlReady(window, id));
            window.addConnection(); QVERIFY(xmlReady(window, id)); profile = window.selectedId(); QVERIFY(!profile.isEmpty());
            setGridText(window, "name", "XML fixture connection"); QVERIFY(xmlReady(window, id));
            setGridText(window, "host", "127.0.0.1"); QVERIFY(xmlReady(window, id)); setGridPort(window, port); QVERIFY(xmlReady(window, id));
            setGridText(window, "username", "rdpfixture"); QVERIFY(xmlReady(window, id));
            setGridChoice(window, "resolution", "800x600"); QVERIFY(xmlReady(window, id));
            setGridChoice(window, "automatic_resize", false); QVERIFY(xmlReady(window, id));
            setGridText(window, "password", "synthetic-replaced-password"); QVERIFY(acceptXmlPasswordReview(window)); QVERIFY(xmlReady(window, id)); QVERIFY(!window.findChild<QDialog*>("filePassword"));
            if (custom) {
                QVERIFY(window.selectConnectionFile(id)); setGridChoice(window, "protection", true); QVERIFY(enterXmlMaster(window, master, true)); QVERIFY(xmlReady(window, id));
                QVERIFY(window.selectConnectionProfile({id, profile}));
            }
            const auto before = contents(path); setGridText(window, "password", QString::fromUtf8(password)); QVERIFY(acceptXmlPasswordReview(window)); QVERIFY(xmlReady(window, id));
            QVERIFY(contents(path) != before); assertXmlSecretFree(window, password, master);
            if (full) { QVERIFY(window.selectConnectionFile(id)); setGridChoice(window, "full_encryption", true); QVERIFY(confirmXmlProtection(window)); QVERIFY(xmlReady(window, id)); }
            QVERIFY(window.selectConnectionProfile({id, profile})); auto* pane = connectXml(window, {id, profile}, 1); QVERIFY(pane);
            certificate(pane); QVERIFY(!QTest::currentTestFailed()); shellInput(pane); QVERIFY(!QTest::currentTestFailed());
            saved = contents(path); QVERIFY(saved.startsWith("<?xml") && !saved.contains(password) && !saved.contains(master));
            QVERIFY(!QFile::exists(path + ".vault"));
            if (full) QVERIFY(!saved.contains("XML fixture connection") && !saved.contains("127.0.0.1") && !saved.contains("rdpfixture"));
            auto xml = ConnectionXml::read(saved); QVERIFY(xml); QCOMPARE(xml->fullEncryption(), full);
            Secret key(custom ? master : QByteArray("mR3m")); QVERIFY(xml->unlock(key)); QVERIFY(xml->passwordProtected() == custom); auto decoded = xml->password(key, profile, pane->target());
            QVERIFY(decoded && decoded->takeBytes() == password);
            window.closeTab(0); window.close(); QCOMPARE(window.sessionCount(), size_t(0));
        }
        QVERIFY(QFile::copy(path, copy)); QVERIFY(!QFile::exists(copy + ".vault")); QTest::qWait(1000);
        {
            MainWindow restart(copy); QVERIFY(showXmlWindow(restart)); const auto id = restart.connectionFiles().first(); auto* files = restart.findChild<DocumentService*>();
            QTRY_VERIFY(files->status(id) && !files->status(id)->busy); QCOMPARE(files->status(id)->unlocked, !custom);
            QCOMPARE(files->status(id)->metadataAvailable, !full || !custom); QCOMPARE(contents(copy), saved);
            if (full && custom) {
                QVERIFY(restart.selectConnectionFile(id)); restart.findChild<QAction*>("actionUnlockPasswords")->trigger();
                QTRY_VERIFY(restart.findChild<QDialog*>("filePassword")); restart.findChild<QDialog*>("filePassword")->reject();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                QVERIFY(!files->status(id)->metadataAvailable); QCOMPARE(contents(copy), saved);
            }
            QVERIFY(unlockXml(restart, id, master)); QVERIFY(restart.selectConnectionProfile({id, profile}));
            auto* pane = connectXml(restart, {id, profile}, 1); QVERIFY(pane); certificate(pane); QVERIFY(!QTest::currentTestFailed());
            shellInput(pane, Qt::Key_B); QVERIFY(!QTest::currentTestFailed()); QCOMPARE(pane->target().username, QString("rdpfixture"));
            QCOMPARE(pane->findChild<DesktopView*>()->frame().size(), QSize(800, 600)); assertXmlSecretFree(restart, password, master);
            QVERIFY(!restart.findChild<QDialog*>("filePassword") && !restart.findChild<QDialog*>("CredentialsDialog"));
            QVERIFY(captureXmlWindow(restart, full ? "/rdp-xml-full.png" : "/rdp-xml-password-only.png"));
            QCOMPARE(contents(path), saved); QCOMPARE(contents(copy), saved); QVERIFY(!QFile::exists(copy + ".vault"));
            restart.close(); QCOMPARE(restart.sessionCount(), size_t(0));
        }
    }
    void multipleConnectionFilesThroughShell() {
        const auto password = qgetenv("RM_RDP_TEST_PASSWORD"), master = qgetenv("RM_XML_TEST_MASTER");
        const auto password2 = qgetenv("RM_RDP_TEST_PASSWORD_2"), master2 = qgetenv("RM_XML_TEST_MASTER_2");
        const int port = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT"), port2 = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT_2");
        QVERIFY(!password.isEmpty() && !password2.isEmpty() && password != password2 && master.size() >= 12 && master2.size() >= 12 && master != master2);
        QVERIFY(port > 0 && port2 > 0 && port != port2);
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/xml-multiple-XXXXXX"); QVERIFY(dir.isValid());
        const auto first = dir.filePath("first.xml"), second = dir.filePath("second.xml"); QString profile; QByteArray firstBytes, secondBytes;
        {
            MainWindow window(first); QVERIFY(showXmlWindow(window)); const auto id = window.connectionFiles().first(); QVERIFY(xmlReady(window, id));
            window.addConnection(); QVERIFY(xmlReady(window, id)); profile = window.selectedId();
            setGridText(window, "host", "127.0.0.1"); QVERIFY(xmlReady(window, id)); setGridPort(window, port); QVERIFY(xmlReady(window, id));
            setGridText(window, "username", "rdpfixture"); QVERIFY(xmlReady(window, id));
            setGridChoice(window, "resolution", "800x600"); QVERIFY(xmlReady(window, id)); setGridChoice(window, "automatic_resize", false); QVERIFY(xmlReady(window, id));
            setGridText(window, "password", QString::fromUtf8(password)); QVERIFY(acceptXmlPasswordReview(window)); QVERIFY(xmlReady(window, id));
            QVERIFY(window.selectConnectionFile(id)); setGridChoice(window, "protection", true); QVERIFY(enterXmlMaster(window, master, true)); QVERIFY(xmlReady(window, id));
            firstBytes = contents(first); window.close();
        }
        // Copy the portable identity/UUID, then explicitly rekey the test copy before opening it.
        auto xml = ConnectionXml::read(firstBytes); QVERIFY(xml); Secret oldKey(master), newKey(master2);
        QVERIFY(xml->unlock(oldKey) && xml->changeMaster(oldKey, newKey)); const auto copy = xml->serialize(newKey); QVERIFY(copy);
        { QFile output(second); QVERIFY(output.open(QIODevice::WriteOnly)); QVERIFY(output.write(*copy) == copy->size()); }
        {
            MainWindow window(first); QVERIFY(showXmlWindow(window)); const auto a = window.connectionFiles().first(); QVERIFY(unlockXml(window, a, master));
            const auto b = window.openConnectionFile(second); QVERIFY(b.valid() && b != a); QVERIFY(unlockXml(window, b, master2));
            QVERIFY(window.selectConnectionProfile({b, profile})); setGridPort(window, port2); QVERIFY(xmlReady(window, b));
            setGridText(window, "password", QString::fromUtf8(password2)); QVERIFY(acceptXmlPasswordReview(window)); QVERIFY(xmlReady(window, b));
            QVERIFY(window.selectConnectionFile(b)); setGridChoice(window, "full_encryption", true); QVERIFY(confirmXmlProtection(window)); QVERIFY(xmlReady(window, b));
            QCOMPARE(contents(first), firstBytes); secondBytes = contents(second);
            QVERIFY(!QFile::exists(first + ".vault") && !QFile::exists(second + ".vault"));
            QVERIFY(window.selectConnectionProfile({a, profile})); auto* paneA = connectXml(window, {a, profile}, 1); QVERIFY(paneA);
            certificate(paneA); QVERIFY(!QTest::currentTestFailed()); shellInput(paneA, Qt::Key_A); QVERIFY(!QTest::currentTestFailed());
            QVERIFY(window.selectConnectionProfile({b, profile})); auto* paneB = connectXml(window, {b, profile}, 2); QVERIFY(paneB);
            certificate(paneB); QVERIFY(!QTest::currentTestFailed()); shellInput(paneB, Qt::Key_B); QVERIFY(!QTest::currentTestFailed());
            const auto targetA = paneA->target(), targetB = paneB->target(); QVERIFY(targetA.port != targetB.port);
            auto* files = window.findChild<DocumentService*>(); QVERIFY(window.selectConnectionFile(a)); window.findChild<QAction*>("actionLockPasswords")->trigger();
            QVERIFY(!files->status(a)->unlocked && files->status(b)->unlocked); QCOMPARE(window.sessionCount(), size_t(2));
            QVERIFY(paneA->findChild<RdpWorker*>()->isRunning() && paneB->findChild<RdpWorker*>()->isRunning());
            auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); tabs->setCurrentWidget(paneA);
            shellInput(paneA, Qt::Key_E); QVERIFY(!QTest::currentTestFailed());
            QVERIFY(unlockXml(window, a, master)); QVERIFY(window.selectConnectionProfile({a, profile}));
            setGridText(window, "description", "Edited only first.xml"); QVERIFY(xmlReady(window, a));
            QVERIFY(paneA->target() == targetA && paneB->target() == targetB); QCOMPARE(contents(second), secondBytes); firstBytes = contents(first);
            tabs->setCurrentWidget(paneB); shellInput(paneB, Qt::Key_F); QVERIFY(!QTest::currentTestFailed());
            assertXmlSecretFree(window, password, master); assertXmlSecretFree(window, password2, master2);
            QVERIFY(!window.findChild<QDialog*>("filePassword") && !window.findChild<QDialog*>("CredentialsDialog"));
            auto* roots = window.findChild<QTreeWidget*>("connectionTree"); QCOMPARE(roots->topLevelItemCount(), 2);
            QVERIFY(roots->topLevelItem(0)->text(0).contains("first.xml") && roots->topLevelItem(1)->text(0).contains("second.xml"));
            QVERIFY(captureXmlWindow(window, "/rdp-xml-multiple.png")); window.close(); QCOMPARE(window.sessionCount(), size_t(0));
        }
        QTest::qWait(1000);
        {
            MainWindow restart(first); QVERIFY(showXmlWindow(restart)); const auto a = restart.connectionFiles().first(); auto* files = restart.findChild<DocumentService*>();
            QTRY_VERIFY(files->status(a) && !files->status(a)->busy); const auto b = restart.openConnectionFile(second);
            QTRY_VERIFY(files->status(b) && !files->status(b)->busy); QVERIFY(!files->status(a)->unlocked && !files->status(b)->unlocked);
            QVERIFY(files->status(a)->metadataAvailable && !files->status(b)->metadataAvailable);
            QTRY_VERIFY(restart.findChild<QDialog*>("filePassword")); QVERIFY(restart.findChild<QDialog*>("filePassword")->windowTitle().contains("first.xml"));
            restart.findChild<QDialog*>("filePassword")->reject(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QTRY_VERIFY(restart.findChild<QDialog*>("filePassword")); QVERIFY(restart.findChild<QDialog*>("filePassword")->windowTitle().contains("second.xml"));
            restart.findChild<QDialog*>("filePassword")->reject(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCOMPARE(contents(first), firstBytes); QCOMPARE(contents(second), secondBytes);
            QVERIFY(unlockXml(restart, a, master)); QVERIFY(!files->status(b)->unlocked && !files->status(b)->metadataAvailable);
            QVERIFY(restart.selectConnectionProfile({a, profile})); QCOMPARE(gridValue(restart, "description").toString(), QString("Edited only first.xml"));
            auto* paneA = connectXml(restart, {a, profile}, 1); QVERIFY(paneA); certificate(paneA); QVERIFY(!QTest::currentTestFailed());
            shellInput(paneA, Qt::Key_C); QVERIFY(!QTest::currentTestFailed());
            QVERIFY(unlockXml(restart, b, master2)); QVERIFY(restart.selectConnectionProfile({b, profile}));
            auto* paneB = connectXml(restart, {b, profile}, 2); QVERIFY(paneB); certificate(paneB); QVERIFY(!QTest::currentTestFailed());
            shellInput(paneB, Qt::Key_D); QVERIFY(!QTest::currentTestFailed());
            const auto oldSession = paneA->id(), otherSession = paneB->id(); const auto oldTarget = paneA->target(), otherTarget = paneB->target();
            QVERIFY(restart.selectConnectionProfile({a, profile})); setGridChoice(restart, "font_smoothing", !oldTarget.options.fontSmoothing); QVERIFY(xmlReady(restart, a));
            QVERIFY(paneA->target() == oldTarget && paneB->target() == otherTarget); QCOMPARE(contents(second), secondBytes);
            QVERIFY(restart.selectConnectionFile(a)); restart.findChild<QAction*>("actionLockPasswords")->trigger(); QVERIFY(!files->status(a)->unlocked && files->status(b)->unlocked);
            QVERIFY(restart.selectConnectionFile(b)); auto* tabs = restart.findChild<QTabWidget*>("sessionTabs"); tabs->setCurrentWidget(paneA);
            QPointer<SessionPane> retired = paneA; restart.reconnectCurrent(); QVERIFY(enterXmlMaster(restart, master, false));
            QTRY_VERIFY(tabs->count() == 2 && !retired && qobject_cast<SessionPane*>(tabs->currentWidget())->id() != oldSession);
            paneA = qobject_cast<SessionPane*>(tabs->currentWidget()); QVERIFY(paneA && paneA->reference() == (ProfileReference{a, profile}) && paneA->id() != oldSession);
            QVERIFY(!restart.findChild<QDialog*>("CredentialsDialog")); QCOMPARE(paneA->target().options.fontSmoothing, !oldTarget.options.fontSmoothing);
            QCOMPARE(paneB->id(), otherSession); QVERIFY(paneB->target() == otherTarget); certificate(paneA); QVERIFY(!QTest::currentTestFailed());
            shellInput(paneA, Qt::Key_G); QVERIFY(!QTest::currentTestFailed());
            QVERIFY(!QFile::exists(first + ".vault") && !QFile::exists(second + ".vault")); restart.close(); QCOMPARE(restart.sessionCount(), size_t(0));
        }
    }
    void savedConnectionThroughShell() {
        const auto password = qgetenv("RM_RDP_TEST_PASSWORD"); const auto port = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT"); QVERIFY(!password.isEmpty() && port > 0);
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/saved-shell-XXXXXX"); QVERIFY(dir.isValid());
        const auto path = dir.path() + "/confCons.xml"; MainWindow window(path, true); window.show(); WAIT_STORE(window);
        window.findChild<QAction*>("actionAddFolder")->trigger(); WAIT_STORE(window); setGridText(window, "name", "Fixture folder"); WAIT_STORE(window);
        setGridText(window, "username", "rdpfixture"); WAIT_STORE(window);
        window.findChild<QAction*>("actionAddConnection")->trigger(); WAIT_STORE(window); const auto profile = window.selectedId();
        setGridText(window, "host", "127.0.0.1"); WAIT_STORE(window);
        setGridPort(window, port); WAIT_STORE(window); const auto saved = contents(path); QVERIFY(!saved.isEmpty());
        auto* tree = window.findChild<QTreeWidget*>("connectionTree"); auto* tabs = window.findChild<QTabWidget*>("sessionTabs");
        QCOMPARE(window.sessionCount(), size_t(0));
        QTRY_VERIFY(tree->viewport()->rect().contains(tree->visualItemRect(tree->currentItem()).center()));
        const auto point = tree->visualItemRect(tree->currentItem()).center();
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, point); QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QTRY_VERIFY(window.findChild<QDialog*>("CredentialsDialog")); QCOMPARE(tabs->count(), 0); credentials(window, password); QCOMPARE(tabs->count(), 1);
        auto* pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane && !pane->mock()); QCOMPARE(pane->profile(), profile);
        certificate(pane); auto* view = pane->findChild<DesktopView*>(); QVERIFY(view);
        QTRY_VERIFY_WITH_TIMEOUT(!view->frame().isNull(), 30000); auto* worker = pane->findChild<RdpWorker*>(); QVERIFY(worker && worker->isRunning());
        std::atomic<bool> producedOffGui{false}; bool deliveredOnGui = false; auto* gui = qApp->thread();
        connect(worker, &RdpWorker::frameAvailable, this, [&] { producedOffGui = QThread::currentThread() != gui; }, Qt::DirectConnection);
        connect(worker, &RdpWorker::frameAvailable, this, [&] { deliveredOnGui = QThread::currentThread() == gui; }, Qt::QueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(producedOffGui.load() && deliveredOnGui, 10000);
        disconnect(worker, nullptr, this, nullptr); shellInput(pane);
        QVERIFY(window.grab().save(qEnvironmentVariable("RM_BUILD_DIR") + "/rdp-shell.png"));
        const auto old = pane->id(); const auto target = pane->target();
        window.findChild<QAction*>("actionTreeConnect")->trigger(); QCOMPARE(tabs->count(), 1); QVERIFY(!window.findChild<QDialog*>("CredentialsDialog"));
        window.resize(1050, 760); QTRY_COMPARE_WITH_TIMEOUT(view->frame().size(), QSize(view->width() & ~1, view->height()), 15000);
        QPointer<RdpWorker> oldWorker = worker; window.findChild<QAction*>("actionTabDisconnect")->trigger(); QVERIFY(!oldWorker);
        auto* status = pane->findChild<QLabel*>("sessionStatus"); QVERIFY(status);
        QTRY_VERIFY(status->isVisible()); QTRY_VERIFY(status->height() <= status->sizeHint().height());
        QCOMPARE(window.sessionCount(), size_t(1)); QCOMPARE(tabs->count(), 1); QCOMPARE(contents(path), saved);
        QTest::qWait(1000); window.findChild<QAction*>("actionTabReconnect")->trigger(); credentials(window, password);
        pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane && pane->id() != old); QVERIFY(pane->target() == target);
        certificate(pane); QTRY_VERIFY_WITH_TIMEOUT(!pane->findChild<DesktopView*>()->frame().isNull(), 30000);
        QCOMPARE(contents(path), saved); QVERIFY(!saved.contains(password));
        QPointer<RdpWorker> freshWorker = pane->findChild<RdpWorker*>(); window.close(); QVERIFY(!freshWorker); QCOMPARE(window.sessionCount(), size_t(0));
        QCOMPARE(contents(path), saved); QTest::qWait(1000);
    }
    void fullscreenThroughShell_data() {
        QTest::addColumn<bool>("automatic"); QTest::newRow("fixed") << false; QTest::newRow("automatic") << true;
    }
    void fullscreenThroughShell() {
        QFETCH(bool, automatic); const auto password = qgetenv("RM_RDP_TEST_PASSWORD"); const auto port = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT"); QVERIFY(!password.isEmpty() && port > 0);
        QTemporaryDir dir; const auto path = dir.filePath("profiles.json"); MainWindow window(path, true); window.resize(900, 650); QVERIFY(showXmlWindow(window)); WAIT_STORE(window);
        window.addConnection(); WAIT_STORE(window); setGridText(window, "name", "Owned fullscreen fixture"); WAIT_STORE(window);
        setGridText(window, "host", "127.0.0.1"); WAIT_STORE(window); setGridPort(window, port); WAIT_STORE(window); setGridText(window, "username", "rdpfixture"); WAIT_STORE(window);
        setGridChoice(window, "resolution", "800x600"); WAIT_STORE(window); setGridChoice(window, "automatic_resize", automatic); WAIT_STORE(window);
        const auto saved = contents(path); window.connectSelected(); credentials(window, password);
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); auto* pane = qobject_cast<SessionPane*>(tabs->currentWidget()); QVERIFY(pane);
        certificate(pane); QVERIFY(!QTest::currentTestFailed()); auto* view = pane->findChild<DesktopView*>(); QTRY_VERIFY_WITH_TIMEOUT(fixtureReady(view), 30000);
        const auto target = pane->target(); const auto session = pane->id(); auto* worker = pane->findChild<RdpWorker*>(); QVERIFY(worker && worker->isRunning());
        auto* fullscreen = window.findChild<QAction*>("actionTabFullscreen"); QVERIFY(fullscreen->isEnabled()); fullscreen->trigger();
        QPointer<QMainWindow> full; QTRY_VERIFY((full = window.sessionFullscreenWindow()) && full->isFullScreen());
        QTRY_VERIFY(full->findChild<QWidget*>("fullscreenBar")->isVisible()); QVERIFY(window.findChild<QDockWidget*>("connectionsDock")->isVisible());
        QVERIFY(!window.isFullScreen()); QVERIFY(full->isAncestorOf(pane));
        if (automatic) QTRY_COMPARE_WITH_TIMEOUT(view->frame().size(), QSize(view->width() & ~1, view->height()), 15000);
        else QCOMPARE(view->frame().size(), QSize(800, 600));
        shellInput(pane); QVERIFY(!QTest::currentTestFailed());
        QVERIFY(!window.findChild<QDialog*>("CredentialsDialog") && !pane->findChild<QMessageBox*>("rdpCertificate") && !window.findChild<PropertyDelegate*>()->activeEditor());
        QVERIFY(full->grab().save(qEnvironmentVariable("RM_BUILD_DIR") + (automatic ? "/rdp-fullscreen-automatic.png" : "/rdp-fullscreen-fixed.png")));
        // Minimize hides only the session's window; restoring it returns to fullscreen.
        QSignalSpy pointers(view, &DesktopView::pointerInput); QTest::mouseClick(full->findChild<QToolButton*>("minimizeFullscreenConnection"), Qt::LeftButton);
        QTRY_VERIFY(full->isMinimized()); QVERIFY(!window.isMinimized()); QVERIFY(worker->isRunning() && pane->active()); QCOMPARE(pointers.size(), 0);
        full->showNormal(); QTRY_VERIFY(full->isFullScreen()); QTRY_VERIFY(view->isVisible()); full->raise(); full->activateWindow(); view->setFocus(); QTRY_VERIFY(view->hasFocus());
        QSignalSpy keys(view, &DesktopView::keyInput); QTest::mouseClick(full->findChild<QToolButton*>("exitSessionFullscreen"), Qt::LeftButton);
        QTRY_VERIFY(!window.sessionFullscreenWindow()); QCOMPARE(tabs->currentWidget(), pane);
        QCOMPARE(keys.size(), 0); QCOMPARE(pane->findChild<RdpWorker*>(), worker); QCOMPARE(pane->findChild<DesktopView*>(), view);
        QTRY_VERIFY(window.findChild<QDockWidget*>("connectionsDock")->isVisible()); QCOMPARE(pane->id(), session); QCOMPARE(pane->target(), target); QCOMPARE(contents(path), saved);
        if (automatic) QTRY_COMPARE_WITH_TIMEOUT(view->frame().size(), QSize(view->width() & ~1, view->height()), 15000);
        else QCOMPARE(view->frame().size(), QSize(800, 600));
        fullscreen->trigger(); QTRY_VERIFY((full = window.sessionFullscreenWindow()) && full->isFullScreen());
        QTest::mouseClick(full->findChild<QToolButton*>("closeFullscreenConnection"), Qt::LeftButton);
        QTRY_COMPARE(window.sessionCount(), size_t(0)); QTRY_VERIFY(!window.sessionFullscreenWindow()); QCOMPARE(tabs->count(), 0); QCOMPARE(window.sessionCount(), size_t(0)); QCOMPARE(contents(path), saved);
        window.close(); QTest::qWait(1000);
    }
    void dragSessionBetweenWindowsThroughShell() {
        const auto password = qgetenv("RM_RDP_TEST_PASSWORD"); const auto port = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT"); QVERIFY(!password.isEmpty() && port > 0);
        QTemporaryDir dir; MainWindow window(dir.filePath("confCons.xml"), true); window.resize(1000, 700); QVERIFY(showXmlWindow(window)); WAIT_STORE(window);
        window.quickConnect("127.0.0.1:" + QString::number(port)); credentials(window, password);
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); QPointer<SessionPane> pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane);
        certificate(pane); QVERIFY(!QTest::currentTestFailed());
        auto* view = pane->findChild<DesktopView*>(); QPointer<RdpWorker> worker = pane->findChild<RdpWorker*>(); const auto session = pane->id();
        // Tear the live session into a new window: same session, worker and view, no reconnect.
        dragTab(tabs->tabBar(), 0, {window.mapToGlobal(QPoint(500, 450)), window.mapToGlobal(QPoint(540, 470))});
        QTRY_COMPARE(panelWindows().size(), 1); auto* popped = panelWindows().first(); QCOMPARE(tabs->count(), 0);
        auto* moved = popped->findChild<QTabWidget*>("panelSessionTabs"); QCOMPARE(moved->widget(0), pane.data());
        QCOMPARE(pane->id(), session); QCOMPARE(pane->findChild<RdpWorker*>(), worker.data()); QCOMPARE(pane->findChild<DesktopView*>(), view); QVERIFY(worker->isRunning());
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() == "cocoa") activateNativeTestApplication(popped);
#endif
        popped->raise(); popped->activateWindow(); QTRY_VERIFY_WITH_TIMEOUT(fixtureReady(view), 15000);
        shellInput(pane); QVERIFY(!QTest::currentTestFailed());
        QVERIFY(popped->grab().save(qEnvironmentVariable("RM_BUILD_DIR") + "/rdp-dragged-window.png"));
        // Drag it back onto the main window's tab strip: the dragged-out window disappears.
        const auto strip = tabs->mapToGlobal(QPoint(tabs->width() / 2, tabs->tabBar()->y() + tabs->tabBar()->height() / 2));
        dragTab(moved->tabBar(), 0, {moved->tabBar()->mapToGlobal(QPoint(10, -80)), strip});
        QTRY_VERIFY(panelWindows().isEmpty()); QCOMPARE(tabs->indexOf(pane), 0); QCOMPARE(pane->id(), session); QVERIFY(worker && worker->isRunning());
        QVERIFY(showXmlWindow(window)); shellInput(pane, Qt::Key_B); QVERIFY(!QTest::currentTestFailed());
        // Ending the session after its view lived in another window must not touch a stale GPU context.
        window.closeTab(0); QTRY_VERIFY(pane.isNull()); QVERIFY(!worker); QCOMPARE(window.sessionCount(), size_t(0));
        window.close(); QTest::qWait(500);
    }
    void automaticResizeHonorsProfile_data() {
        QTest::addColumn<bool>("automatic"); QTest::newRow("disabled") << false; QTest::newRow("enabled") << true;
    }
    void automaticResizeHonorsProfile() {
        QFETCH(bool, automatic); const auto password = qgetenv("RM_RDP_TEST_PASSWORD"); const auto port = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT"); QVERIFY(!password.isEmpty() && port > 0);
        QTemporaryDir dir; MainWindow window(dir.filePath("confCons.xml"), true); window.show(); WAIT_STORE(window); window.addConnection(); WAIT_STORE(window);
        setGridText(window, "host", "127.0.0.1"); WAIT_STORE(window); setGridPort(window, port); WAIT_STORE(window); setGridText(window, "username", "rdpfixture"); WAIT_STORE(window);
        setGridChoice(window, "resolution", "800x600"); WAIT_STORE(window); setGridChoice(window, "automatic_resize", automatic); WAIT_STORE(window); setGridChoice(window, "color_depth", automatic ? 24 : 16); WAIT_STORE(window);
        window.connectSelected(); credentials(window, password); auto* pane = qobject_cast<SessionPane*>(window.findChild<QTabWidget*>("sessionTabs")->widget(0)); QVERIFY(pane);
        // Without a certificate prompt negotiation can finish before a spy attaches; read the worker's record.
        auto* worker = pane->findChild<RdpWorker*>(); QVERIFY(worker); certificate(pane); QTRY_VERIFY_WITH_TIMEOUT(worker->negotiatedDesktop().isValid(), 30000);
        QCOMPARE(worker->negotiatedDesktop(), QSize(800, 600)); QCOMPARE(worker->negotiatedColorDepth(), automatic ? 24U : 16U);
        auto* view = pane->findChild<DesktopView*>(); QTRY_VERIFY_WITH_TIMEOUT(!view->frame().isNull(), 30000);
        if (!automatic) QCOMPARE(view->frame().size(), QSize(800, 600));
        window.resize(1120, 810); QSignalSpy frames(pane->findChild<RdpWorker*>(), &RdpWorker::frameAvailable);
        QTRY_VERIFY_WITH_TIMEOUT(frames.size() > 10, 10000);
        if (automatic) QTRY_COMPARE_WITH_TIMEOUT(view->frame().size(), QSize(view->width() & ~1, view->height()), 15000);
        else QCOMPARE(view->frame().size(), QSize(800, 600));
        window.close(); QCOMPARE(window.sessionCount(), size_t(0));
    }
    void fixedResolutionPreservesInputMapping() {
        const auto password = qgetenv("RM_RDP_TEST_PASSWORD"); const auto port = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT"); QVERIFY(!password.isEmpty() && port > 0);
        QTemporaryDir dir; MainWindow window(dir.filePath("confCons.xml"), true); window.resize(900, 650); window.show(); WAIT_STORE(window); window.addConnection(); WAIT_STORE(window);
        setGridText(window, "host", "127.0.0.1"); WAIT_STORE(window); setGridPort(window, port); WAIT_STORE(window); setGridText(window, "username", "rdpfixture"); WAIT_STORE(window);
        setGridChoice(window, "resolution", "1280x720"); WAIT_STORE(window); setGridChoice(window, "automatic_resize", false); WAIT_STORE(window);
        window.connectSelected(); credentials(window, password); auto* pane = qobject_cast<SessionPane*>(window.findChild<QTabWidget*>("sessionTabs")->widget(0)); QVERIFY(pane); certificate(pane);
        auto* view = pane->findChild<DesktopView*>();
        auto* worker = pane->findChild<RdpWorker*>(); QVERIFY(worker);
        auto diagnostics = std::make_shared<int>(0); QPointer<DesktopView> guardedView = view;
        connect(worker, &RdpWorker::frameAvailable, this, [diagnostics, guardedView] {
            if (!guardedView || (*diagnostics)++ >= 8) return;
            const auto image = guardedView->frame();
            qInfo("RM_FIXED_FRAME=%d,%d,%u", image.width(), image.height(), image.isNull() ? 0 : unsigned(image.pixel(0, 0) & 0xffffff));
        }, Qt::QueuedConnection);
        connect(pane, &SessionPane::state, this, [](const QString&, const QString& state) {
            if (state == "failed") qInfo("RM_FIXED_STATE=1");
            if (state == "disconnected") qInfo("RM_FIXED_STATE=2");
        });
        QTRY_COMPARE_WITH_TIMEOUT(view->frame().size(), QSize(1280, 720), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(fixtureReady(view), 15000);
        for (int fraction : {1, 3}) {
            if (fraction == 3) { window.resize(1150, 850); QTest::qWait(400); }
            QCOMPARE(view->frame().size(), QSize(1280, 720)); QVERIFY(view->size() != view->frame().size());
            const auto scaled = QSize(1280, 720).scaled(view->size(), Qt::KeepAspectRatio);
            const QRect area(QPoint((view->width() - scaled.width()) / 2, (view->height() - scaled.height()) / 2), scaled);
            QSignalSpy pointers(view, &DesktopView::pointerInput); QSignalSpy frames(pane->findChild<RdpWorker*>(), &RdpWorker::frameAvailable);
            if (area.top() > 0) { QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier, QPoint(area.center().x(), 0)); QVERIFY(pointers.isEmpty()); }
            QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier, QPoint(area.left() + area.width() * fraction / 4, area.top() + area.height() * fraction / 4));
            QVERIFY(pointers.size() >= 2); const auto down = pointers.at(pointers.size() - 2);
            QVERIFY(qAbs(down.at(1).toInt() - 320 * fraction) <= 3); QVERIFY(qAbs(down.at(2).toInt() - 180 * fraction) <= 3);
            QTest::keyClick(view, Qt::Key_A); QTRY_VERIFY_WITH_TIMEOUT(frames.size() > 3, 10000);
            presentedFixture(view);
        }
        window.close(); QCOMPARE(window.sessionCount(), size_t(0));
    }
    void savedXmlCredentialsThroughShell() {
        const auto password = qgetenv("RM_RDP_TEST_PASSWORD"), master = qgetenv("RM_XML_TEST_MASTER");
        const auto port = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT"); QVERIFY(!password.isEmpty() && master.size() >= 12 && port > 0);
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/saved-password-shell-XXXXXX"); QVERIFY(dir.isValid());
        const auto path = dir.filePath("confCons.xml"); QByteArray saved; QString profile;
        {
            MainWindow window(path, true); QVERIFY(showXmlWindow(window)); WAIT_STORE(window); window.addConnection(); WAIT_STORE(window); profile = window.selectedId();
            const auto id = window.selectedDocument();
            setGridText(window, "host", "127.0.0.1"); WAIT_STORE(window); setGridPort(window, port); WAIT_STORE(window);
            setGridText(window, "username", "rdpfixture"); WAIT_STORE(window);
            setGridText(window, "password", QString::fromUtf8(password)); QVERIFY(acceptXmlPasswordReview(window)); WAIT_STORE(window);
            QVERIFY(window.selectConnectionFile(id)); setGridChoice(window, "protection", true); QVERIFY(enterXmlMaster(window, master, true)); WAIT_STORE(window);
            QVERIFY(window.selectConnectionProfile({id, profile})); saved = contents(path);
            auto* pane = connectXml(window, {id, profile}, 1); QVERIFY(pane); QCOMPARE(pane->target().username, QString("rdpfixture")); certificate(pane);
            QTRY_VERIFY_WITH_TIMEOUT(!pane->findChild<DesktopView*>()->frame().isNull(), 30000); shellInput(pane); QVERIFY(!QTest::currentTestFailed());
            QVERIFY(!saved.contains(password) && !saved.contains(master)); assertXmlSecretFree(window, password, master);
            window.findChild<QAction*>("actionLockPasswords")->trigger(); QVERIFY(!window.findChild<DocumentService*>()->status(id)->unlocked);
            QVERIFY(pane->findChild<RdpWorker*>()->isRunning()); QCOMPARE(window.sessionCount(), size_t(1)); QCOMPARE(contents(path), saved);
            window.disconnectCurrent(); QCOMPARE(window.sessionCount(), size_t(1)); window.close(); QCOMPARE(window.sessionCount(), size_t(0));
        }
        QTest::qWait(1000);
        {
            MainWindow window(path, true); QVERIFY(showXmlWindow(window)); const auto id = window.connectionFiles().first(); auto* files = window.findChild<DocumentService*>();
            QTRY_VERIFY(files->status(id) && !files->status(id)->busy); QVERIFY(!files->status(id)->unlocked);
            QVERIFY(enterXmlMaster(window, master, false)); WAIT_STORE(window); QVERIFY(window.selectConnectionProfile({id, profile}));
            window.findChild<QAction*>("actionLockPasswords")->trigger(); QVERIFY(!files->status(id)->unlocked);
            window.connectSelected(); QVERIFY(enterXmlMaster(window, master, false));
            auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); QTRY_COMPARE(tabs->count(), 1); QVERIFY(!window.findChild<QDialog*>("CredentialsDialog"));
            auto* pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane); QCOMPARE(pane->profile(), profile); QCOMPARE(pane->target().username, QString("rdpfixture"));
            certificate(pane); QTRY_VERIFY_WITH_TIMEOUT(!pane->findChild<DesktopView*>()->frame().isNull(), 30000); shellInput(pane); QVERIFY(!QTest::currentTestFailed());
            QSignalSpy clipboard(pane->findChild<RdpWorker*>(), &RdpWorker::remoteClipboard); QTest::qWait(300); QVERIFY(clipboard.isEmpty());
            QVERIFY(captureXmlWindow(window, "/rdp-saved-password-shell.png"));
            QCOMPARE(contents(path), saved); QVERIFY(!QFile::exists(path + ".vault")); window.close(); QCOMPARE(window.sessionCount(), size_t(0));
        }
        QTest::qWait(1000);
    }
    void protectedConnectionsThroughShell() {
        const auto password = qgetenv("RM_RDP_TEST_PASSWORD"), master = qgetenv("RM_XML_TEST_MASTER");
        const auto port = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT"); QVERIFY(!password.isEmpty() && master.size() >= 12 && port > 0);
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/protected-shell-XXXXXX"); QVERIFY(dir.isValid());
        const auto path = dir.filePath("confCons.xml"); QString folder, profile; QByteArray protectedFile;
        auto find = [](ConnectionTree* tree, const QString& id) -> QTreeWidgetItem* {
            for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                if (id.isEmpty() ? (*it)->data(0, Qt::UserRole).canConvert<DocumentRootReference>() : treeProfileId(*it) == id) return *it;
            }
            return nullptr;
        };
        {
            MainWindow window(path, true); window.show(); WAIT_STORE(window); auto* tree = window.findChild<ConnectionTree*>("connectionTree"); QVERIFY(tree);
            window.findChild<QAction*>("actionAddFolder")->trigger(); WAIT_STORE(window); folder = window.selectedId();
            setGridText(window, "name", "Protected fixture folder"); WAIT_STORE(window);
            setGridPort(window, port); WAIT_STORE(window);
            setGridText(window, "username", "rdpfixture"); WAIT_STORE(window);
            setGridChoice(window, "resolution", "800x600"); WAIT_STORE(window); setGridChoice(window, "automatic_resize", false); WAIT_STORE(window);
            setGridChoice(window, "color_depth", 16); WAIT_STORE(window);
            tree->setCurrentItem(find(tree, {})); window.findChild<QAction*>("actionAddConnection")->trigger(); WAIT_STORE(window); profile = window.selectedId();
            setGridText(window, "name", "Protected fixture connection"); WAIT_STORE(window);
            setGridText(window, "description", "Owned loopback acceptance"); WAIT_STORE(window);
            setGridText(window, "host", "127.0.0.1"); WAIT_STORE(window);
            const auto beforeMove = contents(path);
            std::unique_ptr<QMimeData> mime(tree->model()->mimeData({tree->currentIndex()})); QVERIFY(mime);
            tree->scrollToItem(find(tree, folder)); const auto point = tree->visualItemRect(find(tree, folder)).center();
            QDragEnterEvent enter(QPointF(point), Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(tree->viewport(), &enter);
            QDragMoveEvent move(QPointF(point), Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(tree->viewport(), &move);
            QDropEvent drop(point, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(tree->viewport(), &drop); QVERIFY(drop.isAccepted());
            QCOMPARE(contents(path), beforeMove); QCOMPARE(find(tree, profile)->parent(), find(tree, {}));
            QTRY_COMPARE(find(tree, profile)->parent(), find(tree, folder)); WAIT_STORE(window); QCOMPARE(window.selectedId(), profile);
            QCOMPARE(gridValue(window, "host").toString(), QString("127.0.0.1")); QCOMPARE(gridValue(window, "username").toString(), QString("rdpfixture"));
            QCOMPARE(gridValue(window, "resolution").toString(), QString("800x600")); QVERIFY(gridLocal(window, "host") && !gridLocal(window, "resolution"));
            const auto document = storedXml(path); QCOMPARE(document.value("schema_version").toInt(), 2);
            QCOMPARE(document.value("profiles").toArray().size(), 2);
            for (const auto& entry : document.value("profiles").toArray()) if (entry.toObject().value("id") == profile) {
                QCOMPARE(entry.toObject().value("parent").toString(), folder); QCOMPARE(entry.toObject().value("overrides").toObject().value("description").toString(), QString("Owned loopback acceptance"));
            }
            setGridText(window, "password", QString::fromUtf8(password)); QVERIFY(acceptXmlPasswordReview(window)); WAIT_STORE(window);
            auto* pane = connectXml(window, {window.selectedDocument(), profile}, 1); QVERIFY(pane);
            certificate(pane); shellInput(pane); QVERIFY(!QTest::currentTestFailed()); window.closeTab(0);
            tree->setCurrentItem(find(tree, {})); setGridChoice(window, "protection", true);
            QVERIFY(enterXmlMaster(window, master, true)); WAIT_STORE(window);
            setGridChoice(window, "full_encryption", true); QVERIFY(confirmXmlProtection(window)); QTRY_VERIFY(window.documentProtected()); WAIT_STORE(window);
            protectedFile = contents(path); QVERIFY(protectedFile.startsWith("<?xml"));
            QVERIFY(!protectedFile.contains("Protected fixture") && !protectedFile.contains("127.0.0.1") && !protectedFile.contains("rdpfixture"));
            QVERIFY(!protectedFile.contains(password) && !protectedFile.contains(master)); QVERIFY(!QFile::exists(path + ".vault"));
            QVERIFY(captureXmlWindow(window, "/protected-root.png")); window.close(); QCOMPARE(window.sessionCount(), size_t(0));
        }
        QTest::qWait(1000);
        {
            MainWindow window(path, true); window.show(); auto* tree = window.findChild<ConnectionTree*>("connectionTree");
            QTRY_VERIFY(window.findChild<QDialog*>("filePassword")); QCOMPARE(tree->topLevelItemCount(), 1); QCOMPARE(tree->topLevelItem(0)->childCount(), 0);
            QVERIFY(!find(tree, profile) && !find(tree, folder)); QVERIFY(!tree->dragEnabled()); QCOMPARE(contents(path), protectedFile); QVERIFY(!QFile::exists(path + ".vault"));
            QVERIFY(enterXmlMaster(window, master, false)); WAIT_STORE(window);
            QVERIFY(find(tree, folder) && find(tree, profile)); QCOMPARE(find(tree, profile)->parent(), find(tree, folder));
            tree->setCurrentItem(find(tree, profile)); QCOMPARE(gridValue(window, "description").toString(), QString("Owned loopback acceptance"));
            QCOMPARE(gridValue(window, "resolution").toString(), QString("800x600")); QVERIFY(!gridValue(window, "automatic_resize").toBool());
            window.connectSelected(); auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); QTRY_COMPARE(tabs->count(), 1);
            QVERIFY(!window.findChild<QDialog*>("CredentialsDialog")); auto* pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane && !pane->mock());
            QCOMPARE(pane->profile(), profile); QCOMPARE(pane->target().username, QString("rdpfixture")); QVERIFY(pane->target().domain.isEmpty());
            auto* worker = pane->findChild<RdpWorker*>(); QVERIFY(worker); certificate(pane); QTRY_VERIFY_WITH_TIMEOUT(worker->negotiatedDesktop().isValid(), 30000);
            QCOMPARE(worker->negotiatedDesktop(), QSize(800, 600)); QCOMPARE(worker->negotiatedColorDepth(), 16U); shellInput(pane, Qt::Key_B);
            auto* view = pane->findChild<DesktopView*>(); QCOMPARE(view->frame().size(), QSize(800, 600)); const auto captured = pane->target(); const auto session = pane->id();
            QCOMPARE(contents(path), protectedFile); QVERIFY(!QFile::exists(path + ".vault"));
            setGridChoice(window, "resolution", "1024x768"); WAIT_STORE(window); QVERIFY(contents(path) != protectedFile);
            QCOMPARE(pane->id(), session); QVERIFY(pane->target() == captured); window.resize(1120, 810);
            QSignalSpy frames(pane->findChild<RdpWorker*>(), &RdpWorker::frameAvailable); QTRY_VERIFY_WITH_TIMEOUT(frames.size() > 5, 10000); QCOMPARE(view->frame().size(), QSize(800, 600));
            QVERIFY(window.grab().save(qEnvironmentVariable("RM_BUILD_DIR") + "/rdp-protected-shell.png"));
            window.findChild<QAction*>("actionLockPasswords")->trigger(); QVERIFY(!window.findChild<DocumentService*>()->status(window.selectedDocument())->unlocked); QVERIFY(pane->findChild<RdpWorker*>()->isRunning());
            QCOMPARE(find(tree, profile)->parent(), find(tree, folder)); QVERIFY(!QFile::exists(path + ".vault")); protectedFile = contents(path);
            window.close(); QCOMPARE(window.sessionCount(), size_t(0));
        }
        MainWindow restart(path, true); QVERIFY(showXmlWindow(restart)); QVERIFY(enterXmlMaster(restart, master, false)); WAIT_STORE(restart);
        auto* tree = restart.findChild<ConnectionTree*>("connectionTree"); tree->setCurrentItem(find(tree, profile));
        QCOMPARE(gridValue(restart, "resolution").toString(), QString("1024x768")); QCOMPARE(find(tree, profile)->parent(), find(tree, folder));
        QCOMPARE(contents(path), protectedFile); QVERIFY(!QFile::exists(path + ".vault")); restart.close(); QTest::qWait(1000);
    }
    void quickConnectThroughShell() {
        const auto password = qgetenv("RM_RDP_TEST_PASSWORD"); const auto port = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT"); QVERIFY(!password.isEmpty() && port > 0);
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/quick-shell-XXXXXX"); QVERIFY(dir.isValid());
        const auto path = dir.path() + "/confCons.xml"; MainWindow window(path, true); window.show(); WAIT_STORE(window); window.addFolder(); WAIT_STORE(window); const auto original = contents(path); QVERIFY(!original.isEmpty());
        auto* combo = window.findChild<QComboBox*>("quickConnectCombo"); auto* button = window.findChild<QToolButton*>("quickConnectButton");
        combo->setEditText("127.0.0.1:" + QString::number(port)); QTest::mouseClick(button, Qt::LeftButton);
        auto* prompt = window.findChild<QDialog*>("CredentialsDialog"); QVERIFY(prompt); prompt->reject(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(window.sessionCount(), size_t(0)); QCOMPARE(contents(path), original);
        QTest::mouseClick(button, Qt::LeftButton); credentials(window, password);
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); QCOMPARE(tabs->count(), 1); auto* pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane && !pane->mock());
        QVERIFY(pane->profile().isEmpty()); certificate(pane); QTRY_VERIFY_WITH_TIMEOUT(!pane->findChild<DesktopView*>()->frame().isNull(), 30000); shellInput(pane);
        const auto target = pane->target(); const auto old = pane->id(); const auto caption = tabs->tabText(0);
        QTest::mouseClick(button, Qt::LeftButton); credentials(window, password); QCOMPARE(tabs->count(), 1); QCOMPARE(pane->id(), old);
        window.disconnectCurrent(); QCOMPARE(window.sessionCount(), size_t(1)); QCOMPARE(contents(path), original); QTest::qWait(1000);
        window.reconnectCurrent(); credentials(window, password); pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane->id() != old); QVERIFY(pane->target() == target);
        QCOMPARE(tabs->tabText(0), caption); certificate(pane); QTRY_VERIFY_WITH_TIMEOUT(!pane->findChild<DesktopView*>()->frame().isNull(), 30000);
        window.closeTab(0); QCOMPARE(contents(path), original); QCOMPARE(window.sessionCount(), size_t(0)); QTest::qWait(1000);
        QTest::mouseClick(button, Qt::LeftButton); credentials(window, password); pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane);
        certificate(pane); QTRY_VERIFY_WITH_TIMEOUT(!pane->findChild<DesktopView*>()->frame().isNull(), 30000);
        window.findChild<QAction*>("actionSaveAsConnection")->trigger(); auto* save = window.findChild<QDialog*>("saveAsConnection"); QVERIFY(save);
        save->findChild<QLineEdit*>("connectionName")->setText("Saved fixture target"); save->accept(); WAIT_STORE(window);
        QVERIFY(pane->profile().isEmpty()); QVERIFY(pane->target() == target); QCOMPARE(tabs->count(), 1);
        const auto explicitSave = contents(path); QVERIFY(explicitSave != original); QVERIFY(!explicitSave.contains(password));
        QPointer<RdpWorker> worker = pane->findChild<RdpWorker*>(); window.close(); QVERIFY(!worker); QCOMPARE(contents(path), explicitSave); QTest::qWait(1000);
    }
    void closeShellDuringResize() {
        const auto password = qgetenv("RM_RDP_TEST_PASSWORD"); const auto port = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT"); QVERIFY(!password.isEmpty() && port > 0);
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/close-shell-XXXXXX"); QVERIFY(dir.isValid());
        const auto path = dir.path() + "/confCons.xml"; auto window = std::make_unique<MainWindow>(path, true); window->show(); WAIT_STORE((*window));
        window->findChild<QAction*>("actionAddPanel")->trigger();
        window->findChild<QComboBox*>("quickConnectCombo")->setEditText("127.0.0.1:" + QString::number(port));
        QTest::mouseClick(window->findChild<QToolButton*>("quickConnectButton"), Qt::LeftButton); credentials(*window, password);
        auto* groups = window->findChild<QTabWidget*>("groupTabs"); auto* tabs = groups->currentWidget()->findChild<QTabWidget*>();
        QPointer<SessionPane> pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane); certificate(pane);
        auto* view = pane->findChild<DesktopView*>(); QTRY_VERIFY_WITH_TIMEOUT(!view->frame().isNull(), 30000);
        const auto held = view->frame().copy(); const auto pixel = held.pixelColor(0, 0); QPointer<RdpWorker> worker = pane->findChild<RdpWorker*>(); QVERIFY(worker && worker->isRunning());
        QSignalSpy frames(worker, &RdpWorker::frameAvailable); QTRY_VERIFY_WITH_TIMEOUT(frames.size() > 1, 10000);
        // Queue a frame/resize immediately before closing without another GUI event-loop turn.
        groups->setCurrentIndex(0); window->quickConnect("mock.invalid", true); worker->resizeDesktop(QSize(850, 650)); window->resize(1020, 730);
        std::atomic<bool> framePending{false};
        connect(worker, &RdpWorker::frameAvailable, this, [&] { framePending = true; }, Qt::DirectConnection);
        worker->takeFrame();
        for (int i = 0; i < 100 && !framePending.load(); ++i) QThread::msleep(10);
        const bool inFlight = framePending.load(); window->close(); QVERIFY(!worker && !pane); QCOMPARE(window->sessionCount(), size_t(0));
        window.reset(); QCoreApplication::processEvents(); QVERIFY(inFlight); QCOMPARE(held.pixelColor(0, 0), pixel); QVERIFY(!QFile::exists(path)); QTest::qWait(1000);
    }
    void certificateAcceptedWithoutPromptAndReconnect() {
        const auto password = qgetenv("RM_RDP_TEST_PASSWORD"); const auto port = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT");
        QVERIFY(!password.isEmpty() && port > 0);
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/certificate-accept-XXXXXX"); QVERIFY(dir.isValid());
        MainWindow window(dir.path() + "/confCons.xml", true); window.show(); WAIT_STORE(window);
        window.quickConnect("127.0.0.1:" + QString::number(port));
        auto* credentials = window.findChild<QDialog*>("CredentialsDialog"); QVERIFY(credentials);
        QCOMPARE(window.sessionCount(), size_t(0)); credentials->findChild<QLineEdit*>("usernameEdit")->setText("rdpfixture");
        QVERIFY(!credentials->findChild<QCheckBox*>("clipboardCheck")->isChecked());
        credentials->findChild<QLineEdit*>("passwordEdit")->setText(QString::fromUtf8(password)); credentials->accept();
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); QCOMPARE(tabs->count(), 1);
        auto* pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane);
        certificate(pane); QVERIFY(!QTest::currentTestFailed());
        QPointer<RdpWorker> worker = pane->findChild<RdpWorker*>(); QVERIFY(worker && worker->isRunning());
        const auto source = pane->target(); const auto old = pane->id();
        window.disconnectCurrent(); QCOMPARE(tabs->count(), 1); QCOMPARE(window.sessionCount(), size_t(1)); QVERIFY(!worker);
        window.reconnectCurrent(); credentials = window.findChild<QDialog*>("CredentialsDialog"); QVERIFY(credentials);
        QVERIFY(!credentials->findChild<QLineEdit*>("usernameEdit")->isEnabled()); credentials->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(tabs->widget(0), pane); QCOMPARE(window.sessionCount(), size_t(1)); QVERIFY(pane->target() == source);
        window.reconnectCurrent(); credentials = window.findChild<QDialog*>("CredentialsDialog"); QVERIFY(credentials);
        credentials->findChild<QLineEdit*>("passwordEdit")->setText(QString::fromUtf8(password)); credentials->accept();
        pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane); QVERIFY(pane->id() != old); QVERIFY(pane->target() == source);
        certificate(pane); QVERIFY(!QTest::currentTestFailed());
        worker = pane->findChild<RdpWorker*>(); QVERIFY(worker && worker->isRunning());
        window.closeTab(0); QVERIFY(!worker); QCOMPARE(tabs->count(), 0);
        QTest::qWait(1000);
        QVERIFY(!QFile::exists(dir.path() + "/confCons.xml"));
    }
    void unverifiedCertificatesFollowSessionPolicy_data() {
        QTest::addColumn<QString>("policy"); QTest::addColumn<QString>("stored");
        for (const auto& policy : {QString("warn"), QString("always"), QString("required")})
            for (const auto& stored : {QString("new"), QString("changed"), QString("malformed")})
                QTest::newRow(qPrintable(policy + "-" + stored)) << policy << stored;
    }
    void unverifiedCertificatesFollowSessionPolicy() {
        QFETCH(QString, policy); QFETCH(QString, stored);
        const auto password = qgetenv("RM_RDP_TEST_PASSWORD"); const auto port = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT");
        QVERIFY(!password.isEmpty() && port > 0);
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/certificate-policy-XXXXXX"); QVERIFY(dir.isValid());
        // Public test certificate for stale.invalid; its private key was discarded.
        const QByteArray stale = stored == "malformed" ? QByteArray("not a PEM certificate\n") : QByteArray(
            "-----BEGIN CERTIFICATE-----\n"
            "MIIBUTCB+aADAgECAhQf/7gR/B8HUkFyjJhMcghocxKV/zAKBggqhkjOPQQDAjAY\n"
            "MRYwFAYDVQQDDA1zdGFsZS5pbnZhbGlkMB4XDTI2MTAxNTA1MDQ0NFoXDTM2MTAx\n"
            "MjA1MDQ0NFowGDEWMBQGA1UEAwwNc3RhbGUuaW52YWxpZDBZMBMGByqGSM49AgEG\n"
            "CCqGSM49AwEHA0IABLyVcgnLFFyNPdKvlbxCH9F3igyaeVbhuEtDAzWT63kpq/qe\n"
            "K+ySwPMfhkK9bfI6FONPcW2hlH3Vae5wahZOnBmjITAfMB0GA1UdDgQWBBRplAxl\n"
            "WdZgiyBKDu9q6R1gDgIEtTAKBggqhkjOPQQDAgNHADBEAiA4q+vLqSU0E8mlXyrU\n"
            "MISrhVJF/b5NZOfIzUVxnnOPDwIgPo/7HtEIZIr/hX/POLUc1eBS+PNabQtPFrMS\n"
            "fHj/fHI=\n-----END CERTIFICATE-----\n");
        const auto certificatePath = dir.path() + "/server/127.0.0.1_" + QString::number(port) + ".pem";
        if (stored != "new") {
            QVERIFY(QDir().mkpath(dir.path() + "/server")); QFile file(certificatePath);
            QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(stale), qint64(stale.size()));
        }
        RdpConfig config; config.host = "127.0.0.1"; config.port = quint16(port); config.username = "rdpfixture";
        config.password = password; config.configDirectory = dir.path(); config.options.serverAuth = policy;
        RdpWorker worker(std::move(config));
        bool connected = false, failed = false; QString failure;
        connect(&worker, &RdpWorker::stateChanged, this, [&](const QString& state, const QString& detail) {
            connected |= state == "connected";
            if (state == "failed") { failed = true; failure = detail; }
        });
        worker.start(); QTRY_VERIFY_WITH_TIMEOUT(connected || failed, 30000);
        if (policy == "required") {
            QVERIFY(!connected); QVERIFY(failed); QVERIFY(failure.contains("server certificate"));
            QVERIFY(failure.contains("Don't connect"));
        } else { QVERIFY2(!failed, qPrintable(failure)); QVERIFY(connected); }
        worker.stop(); QVERIFY(worker.wait(10000));
        if (stored == "new") QVERIFY(!QFile::exists(certificatePath));
        else QCOMPARE(contents(certificatePath), stale);
        QVERIFY(!QFile::exists(dir.path() + "/known_hosts")); QVERIFY(!QFile::exists(dir.path() + "/known_hosts2"));
        QTest::qWait(1000);
    }
    void panelCloseStopsAllWorkers() {
        const auto password = qgetenv("RM_RDP_TEST_PASSWORD"); const auto port = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT"); QVERIFY(!password.isEmpty() && port > 0);
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/live-panel-XXXXXX"); QVERIFY(dir.isValid());
        MainWindow window(dir.path() + "/confCons.xml", true); window.show(); WAIT_STORE(window);
        window.findChild<QAction*>("actionAddPanel")->trigger(); auto* groups = window.findChild<QTabWidget*>("groupTabs"); QCOMPARE(groups->count(), 2);
        QPointer<QWidget> panel = groups->currentWidget(); auto* tabs = panel->findChild<QTabWidget*>(); QVERIFY(tabs);
        window.quickConnect("127.0.0.1:" + QString::number(port)); auto* credentials = window.findChild<QDialog*>("CredentialsDialog"); QVERIFY(credentials);
        credentials->findChild<QLineEdit*>("usernameEdit")->setText("rdpfixture"); credentials->findChild<QLineEdit*>("passwordEdit")->setText(QString::fromUtf8(password)); credentials->accept();
        QPointer<SessionPane> pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane);
        certificate(pane); QVERIFY(!QTest::currentTestFailed());
        QTRY_VERIFY_WITH_TIMEOUT(!pane->findChild<DesktopView*>()->frame().isNull(), 30000);
        QPointer<RdpWorker> worker = pane->findChild<RdpWorker*>(); QVERIFY(worker && worker->isRunning());
        window.quickConnect("mock.invalid", true); QCOMPARE(tabs->count(), 2); QCOMPARE(window.sessionCount(), size_t(2));
        worker->resizeDesktop(QSize(850, 650));
        const auto point = groups->tabBar()->tabRect(groups->indexOf(panel)).center();
        QContextMenuEvent event(QContextMenuEvent::Mouse, point, groups->tabBar()->mapToGlobal(point)); QApplication::sendEvent(groups->tabBar(), &event);
        auto* menu = window.findChild<QMenu*>("groupContextMenu"); QVERIFY(menu); menu->findChild<QAction*>("actionPanelClose")->trigger(); menu->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!worker && !pane && !panel); QCOMPARE(window.sessionCount(), size_t(0)); QCOMPARE(groups->count(), 1);
        QVERIFY(!QFile::exists(dir.path() + "/confCons.xml")); QTest::qWait(1000);
    }
    void transientSecretsExcludedFromLogs() {
        // The runner checks this process's stdout/stderr for the fixture secret before printing or saving either stream.
        const auto password = qgetenv("RM_RDP_TEST_PASSWORD"); QVERIFY(!password.isEmpty());
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/secrets-XXXXXX"); QVERIFY(dir.isValid());
        const auto path = dir.path() + "/confCons.xml"; MainWindow window(path, true); window.show(); WAIT_STORE(window); window.addConnection(); WAIT_STORE(window);
        setGridText(window, "host", "127.0.0.1"); WAIT_STORE(window);
        window.connectSelected(); QTRY_VERIFY(window.findChild<QDialog*>("CredentialsDialog")); auto* dialog = window.findChild<QDialog*>("CredentialsDialog");
        auto* editor = dialog->findChild<QLineEdit*>("passwordEdit"); editor->setText(QString::fromUtf8(password));
        dialog->findChild<QLineEdit*>("usernameEdit")->setText(QString("invalid\nusername")); dialog->accept();
        QVERIFY(editor->text().isEmpty()); QCOMPARE(window.sessionCount(), size_t(0));
        QVERIFY(!window.lastError().contains(QString::fromUtf8(password)));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(!file.readAll().contains(password));
    }
    void legacyServerStillConnects_data() {
        QTest::addColumn<bool>("legacy");
        QTest::newRow("explicit-legacy") << true;
        QTest::newRow("gfx-request-owned-xrdp") << false;
    }
    void legacyServerStillConnects() {
        QFETCH(bool, legacy);
        const auto password = qgetenv("RM_RDP_TEST_PASSWORD");
        const auto port = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT");
        QVERIFY2(!password.isEmpty() && port > 0, "Run scripts/test-rdp.py against its owned localhost fixture");
        QTemporaryDir directory(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/graphics-policy-XXXXXX");
        QVERIFY(directory.isValid());
        auto trace = std::make_shared<PerformanceTrace>(true);
        RdpConfig config; config.host = "127.0.0.1"; config.port = quint16(port);
        config.username = "rdpfixture"; config.password = password; config.configDirectory = directory.path();
        config.graphicsPolicy = legacy ? RdpGraphicsPolicy::Legacy : RdpGraphicsPolicy::Gfx;
        config.options.resolution = "1280x720";
        config.performance = trace;
        RdpWorker worker(std::move(config)); DesktopView view(false); view.resize(800, 600); view.show();
        bool failed = false, resizeReady = false;
        connect(&worker, &RdpWorker::stateChanged, this, [&](const QString& state, const QString&) { failed |= state == "failed"; });
        connect(&worker, &RdpWorker::resizeAvailable, this, [&](bool available) { resizeReady = available; });
        connect(&worker, &RdpWorker::frameAvailable, this, [&] {
            const auto image = worker.takeFrame(); if (!image.isNull()) view.setFrame(image);
        }, Qt::QueuedConnection);
        worker.start(); QTRY_VERIFY_WITH_TIMEOUT(failed || !view.frame().isNull(), 30000); QVERIFY(!failed);
        QTRY_VERIFY_WITH_TIMEOUT(resizeReady, 15000);
        QTest::qWait(1000);
        for (const auto& sample : trace->samples()) if (sample.stage == PerformanceStage::ReceivedCodec)
            qInfo("RM_COMPAT_CODEC=%u", unsigned(sample.codec));
        QTRY_VERIFY_WITH_TIMEOUT(fixtureReady(&view), 15000);
        worker.resizeDesktop(QSize(1124, 768)); QTRY_COMPARE_WITH_TIMEOUT(view.frame().size(), QSize(1124, 768), 15000);
        QVERIFY(!failed);
        if (QGuiApplication::platformName() != "offscreen") presentedFixture(&view);
        worker.stop(); QVERIFY(worker.wait(10000));
        bool planar = false;
        for (const auto& sample : trace->samples()) if (sample.stage == PerformanceStage::ReceivedCodec) {
            planar |= sample.codec == ReceivedCodec::Planar;
            QVERIFY(sample.codec != ReceivedCodec::Avc420 && sample.codec != ReceivedCodec::Avc444 && sample.codec != ReceivedCodec::Avc444v2);
        }
        QVERIFY(planar); QCOMPARE(trace->dropped(), quint64(0));
        QTest::qWait(1000);
    }
    void liveFramesInputClipboardResizeAndReconnect() {
        const auto password = qgetenv("RM_RDP_TEST_PASSWORD");
        const auto port = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT");
        QVERIFY2(!password.isEmpty() && port > 0, "Run scripts/test-rdp.py; this test only targets its owned localhost fixture");
        for (int attempt = 0; attempt < 2; ++attempt) {
            RdpConfig config; config.host = "127.0.0.1"; config.port = quint16(port);
            config.username = "rdpfixture"; config.password = password; config.clipboard = true;
            config.configDirectory = qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/rdp-integration";
            auto worker = std::make_unique<RdpWorker>(std::move(config));
            bool connected = false, resizeReady = false, ended = false;
            int frames = 0; QImage latest;
            DesktopView view(false); view.resize(800,600); view.show();
            connect(worker.get(), &RdpWorker::stateChanged, this, [&](const QString& state, const QString& detail) { if (state == "connected") connected = true; if (state == "failed" || state == "disconnected") ended = true; if (state == "failed") qWarning() << detail; }, Qt::QueuedConnection);
            connect(worker.get(), &RdpWorker::resizeAvailable, this, [&](bool ready) { resizeReady = ready; }, Qt::QueuedConnection);
            std::atomic<bool> producedOffGui{false};
            auto* guiThread = QCoreApplication::instance()->thread();
            connect(worker.get(), &RdpWorker::frameAvailable, this, [&] { producedOffGui = QThread::currentThread() != guiThread; }, Qt::DirectConnection);
            QPointer<RdpWorker> guarded = worker.get();
            connect(worker.get(), &RdpWorker::frameAvailable, this, [&, guarded] { QCOMPARE(QThread::currentThread(), guiThread); if (guarded) { latest = guarded->takeFrame(); view.setFrame(latest); ++frames; } }, Qt::QueuedConnection);
            QSignalSpy clipboard(worker.get(), &RdpWorker::remoteClipboard);
            worker->start();
            QTRY_VERIFY_WITH_TIMEOUT(connected, 30000);
            QTRY_VERIFY_WITH_TIMEOUT(frames > 1 && !latest.isNull(), 30000);
            QVERIFY(producedOffGui.load());
            QTRY_VERIFY_WITH_TIMEOUT(resizeReady, 15000);
            worker->resizeDesktop(QSize(800, 600));
            QTRY_COMPARE_WITH_TIMEOUT(latest.size(), QSize(800, 600), 15000);
            worker->pointer(0x8000 | 0x1000, 100, 100);
            worker->pointer(0x1000, 100, 100);
            worker->key(0x1e, true, false); worker->key(0x1e, false, false);
            if (attempt == 0) {
                QTRY_VERIFY_WITH_TIMEOUT(!clipboard.empty(), 15000);
                QCOMPARE(clipboard.last().at(0).toString(), QString("server-clipboard-fixture"));
                worker->localClipboard("client-clipboard-fixture");
                QTest::qWait(2000);
            }
            QVERIFY(latest.save(qEnvironmentVariable("RM_BUILD_DIR") + "/rdp-live.png"));
            if (QGuiApplication::platformName() != "offscreen") {
                presentedFixture(&view); const QImage desktop = view.grabFramebuffer(); QVERIFY(!desktop.isNull());
                QVERIFY(desktop.save(qEnvironmentVariable("RM_BUILD_DIR") + "/rdp-widget.png"));
            }
            const QImage held = latest.copy(); const QColor pixel = held.pixelColor(0, 0);
            worker->resizeDesktop(QSize(900, 700));
            QTRY_VERIFY_WITH_TIMEOUT(latest.size() == QSize(900, 700) || ended, 15000);
            QCOMPARE(latest.size(), QSize(900, 700));
            QVERIFY(!ended);
            worker->resizeDesktop(QSize(850, 650));
            worker->stop(); QVERIFY(worker->wait(10000));
            disconnect(worker.get(), nullptr, this, nullptr);
            worker.reset();
            QCoreApplication::processEvents();
            QCOMPARE(held.pixelColor(0, 0), pixel);
            // xrdp's Xorg session outlives the transport; let its pending resize settle.
            QTest::qWait(1000);
        }
    }
};
QTEST_MAIN(RdpIntegration)
#include "rdp_integration.moc"
