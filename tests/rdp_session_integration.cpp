#include "MainWindow.h"
#include "SessionPane.h"
#include "RdpWorker.h"
#include "Views.h"
#include "Appearance.h"
#include "ConnectionXml.h"
#include "DocumentService.h"
#include "ClipboardDriver.h"
#include "GridDriver.h"
#include <QAction>
#include <QContextMenuEvent>
#include <QDialog>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QAbstractButton>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>

class SessionIntegration : public QObject {
    Q_OBJECT
    const QString profile_ = "b5a39219-b8ee-44fa-8728-8d18a577b4c5";
    bool createConnection(const QString& path, bool clipboard = false) {
        const auto password = qgetenv("RM_RDP_TEST_PASSWORD"); const int port = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT");
        if (password.isEmpty() || port <= 0 || port > 65535) return false;
        const Secret master(QByteArray("mR3m")); auto xml = ConnectionXml::create(); if (!xml) return false;
        SessionTarget target{"127.0.0.1", quint16(port), "rdpfixture"}; target.options.resolution = "800x600"; target.options.automaticResize = false;
        const QJsonObject profile{{"id", profile_}, {"name", "Owned session fixture"}, {"kind", "connection"}, {"parent", QJsonValue::Null},
            {"overrides", QJsonObject{{"protocol", "rdp"}, {"host", target.host}, {"port", port}, {"username", target.username},
                {"resolution", "800x600"}, {"automatic_resize", false}, {"clipboard", clipboard}}}};
        if (!xml->patch(master, QJsonDocument(QJsonObject{{"schema_version", 2}, {"profiles", QJsonArray{profile}}}).toJson(QJsonDocument::Compact)) ||
            !xml->setPassword(master, profile_, Secret(password)) || !xml->authorizePasswords(master, profile_, {{profile_, target}})) return false;
        auto bytes = xml->serialize(master); if (!bytes || bytes->contains(password)) return false;
        QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(*bytes) == bytes->size();
    }
    bool show(MainWindow& window) {
        window.resize(1100, 850); window.show();
        if (QGuiApplication::platformName() == "offscreen") return true;
        if (!QTest::qWaitForWindowExposed(&window)) return false;
#ifdef Q_OS_MACOS
        activateNativeTestApplication(&window);
#endif
        window.raise(); window.activateWindow(); return QTest::qWaitForWindowActive(&window);
    }
    SessionPane* connectSaved(MainWindow& window) {
        if (!QTest::qWaitFor([&] { return !window.connectionFiles().isEmpty() &&
            window.findChild<DocumentService*>()->status(window.connectionFiles().first())->metadataAvailable; }, 15000)) return nullptr;
        if (!window.selectConnectionProfile({window.connectionFiles().first(), profile_})) return nullptr;
        window.connectSelected();
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs");
        if (!QTest::qWaitFor([&] { return tabs->count() == 1 || window.findChild<QDialog*>("CredentialsDialog"); }, 15000) ||
            window.findChild<QDialog*>("CredentialsDialog")) return nullptr;
        auto* pane = qobject_cast<SessionPane*>(tabs->currentWidget());
        if (!pane || !acceptCertificate(pane)) return nullptr;
        return ready(pane) ? pane : nullptr;
    }
    bool acceptCertificate(SessionPane* pane) {
        // Server certificates are accepted without a prompt.
        return !pane->findChild<QMessageBox*>("rdpCertificate");
    }
    bool ready(SessionPane* pane) {
        auto* view = pane->findChild<DesktopView*>();
        if (!QTest::qWaitFor([&] { return pane->clipboardSharingAvailable() && view->frame().size() == QSize(800, 600) &&
            view->frame().pixelColor(0, 0).green() >= 70; }, 20000)) return false;
        view->setFocus();
        if (QGuiApplication::platformName() != "offscreen") {
            QSignalSpy frames(view, &DesktopView::framePresented);
            if (!QTest::qWaitFor([&] { return !frames.isEmpty(); }, 10000) || !view->rendererAvailable()) return false;
        }
        return true;
    }
    bool sharing(MainWindow& window, SessionPane* pane, bool enabled) {
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); auto* bar = tabs->tabBar();
        const QPoint position = bar->tabRect(tabs->indexOf(pane)).center();
        QContextMenuEvent event(QContextMenuEvent::Mouse, position, bar->mapToGlobal(position)); QApplication::sendEvent(bar, &event);
        auto* menu = window.findChild<QMenu*>("sessionContextMenu"); if (!menu) return false;
        auto* action = menu->findChild<QAction*>("actionSessionClipboard");
        const bool valid = action && action->isCheckable() && action->isEnabled() && action->isChecked() == pane->clipboardSharingEnabled();
        if (valid && action->isChecked() != enabled) action->trigger();
        menu->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (!show(window)) return false;
        auto* view = pane->findChild<DesktopView*>(); view->setFocus();
        if (QGuiApplication::platformName() != "offscreen" && !QTest::qWaitFor([&] {
            return view->hasFocus() && view->isActiveWindow() && QGuiApplication::applicationState() == Qt::ApplicationActive;
        }, 5000)) return false;
        return valid && pane->clipboardSharingEnabled() == enabled;
    }
    QPoint position(DesktopView* view, QPoint remote) {
        const QSize size = view->frame().size().scaled(view->size(), Qt::KeepAspectRatio);
        const QPoint origin((view->width() - size.width()) / 2, (view->height() - size.height()) / 2);
        return origin + QPoint(qRound(qreal(remote.x()) * size.width() / 800), qRound(qreal(remote.y()) * size.height() / 600));
    }
    void move(DesktopView* view, QPoint remote) {
        const auto local = position(view, remote);
        QTest::mouseMove(view->window()->windowHandle(), view->mapTo(view->window(), local));
    }
private slots:
    void initTestCase() { applyLightAppearance(*qApp); qApp->setQuitOnLastWindowClosed(false); }
    void remoteCursorsThroughShell() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("confCons.xml"); QVERIFY(createConnection(path));
        auto host = std::make_shared<TestClipboardEndpoint>(); MainWindow window(path, false, nullptr, host); QVERIFY(show(window));
        auto* pane = connectSaved(window); QVERIFY(pane); auto* view = pane->findChild<DesktopView*>();
        move(view, {760, 560}); QTest::qWait(200);
        QList<QImage> images;
        for (QPoint remote : {QPoint(150, 140), QPoint(370, 140), QPoint(630, 140), QPoint(150, 280), QPoint(370, 280)}) {
            move(view, remote); QTest::qWait(200);
            QCursor cursor; QImage image;
            QVERIFY(QTest::qWaitFor([&] {
                cursor = view->cursor(); image = cursor.pixmap().toImage();
                return cursor.shape() == Qt::BitmapCursor && !image.isNull() && (images.isEmpty() || image != images.last());
            }, 10000));
            QVERIFY(image.width() > 0 && image.height() > 0); QVERIFY(cursor.hotSpot().x() >= 0 && cursor.hotSpot().y() >= 0);
            QVERIFY(cursor.hotSpot().x() < image.width() / image.devicePixelRatio() && cursor.hotSpot().y() < image.height() / image.devicePixelRatio());
            images.append(image);
        }
        move(view, {150, 140}); QTRY_COMPARE_WITH_TIMEOUT(view->cursor().pixmap().toImage(), images.first(), 10000);
        QCOMPARE(host->reads, 0); QCOMPARE(host->writes, 0); QCOMPARE(host->watches, 0);
        window.disconnectCurrent(); QCOMPARE(view->cursor().shape(), Qt::ArrowCursor);
        QTest::qWait(100); QCOMPARE(view->cursor().shape(), Qt::ArrowCursor);
        window.closeTab(0); QCOMPARE(window.sessionCount(), size_t(0)); window.close();
    }
    void bidirectionalTextClipboardThroughShell() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("confCons.xml"); QVERIFY(createConnection(path));
        auto host = std::make_shared<TestClipboardEndpoint>();
        host->text = QString::fromUtf8("host-session-clipboard\nemoji: \xf0\x9f\x98\x80");
        MainWindow window(path, false, nullptr, host); QVERIFY(show(window)); auto* pane = connectSaved(window); QVERIFY(pane);
        auto* view = pane->findChild<DesktopView*>(); QVERIFY(!pane->clipboardSharingEnabled());
        QCOMPARE(host->reads, 0); QCOMPARE(host->writes, 0); QCOMPARE(host->watches, 0);
        QVERIFY(sharing(window, pane, true)); QTRY_VERIFY(host->reads > 0);
        QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier, position(view, {400, 400}));
        QTest::qWait(500); // Let the server receive the initial format offer before asking its widget to paste.
        QSignalSpy keys(view, &DesktopView::keyInput); QTest::keyClick(view, Qt::Key_F8); QCOMPARE(keys.count(), 2);
        QVERIFY2(QTest::qWaitFor([&] { return host->text == std::optional<QString>("paste-verified"); }, 15000),
            qPrintable(QString("Clipboard paste failed: reads=%1 writes=%2 focused=%3 active=%4 connected=%5")
                .arg(host->reads).arg(host->writes).arg(view->hasFocus()).arg(view->isActiveWindow()).arg(pane->clipboardSharingAvailable()) +
                QString(" initial=%1 host-copy=%2 mismatch=%3 unavailable=%4")
                    .arg(host->text == std::optional<QString>("server-session-initial"))
                    .arg(host->text == std::optional<QString>(QString::fromUtf8("host-session-clipboard\nemoji: \xf0\x9f\x98\x80")))
                    .arg(host->text == std::optional<QString>("paste-mismatch")).arg(host->text == std::optional<QString>("paste-unavailable"))));
        QTest::keyClick(view, Qt::Key_F6);
        QTRY_VERIFY_WITH_TIMEOUT(host->text == std::optional<QString>(QString::fromUtf8("server-session-clipboard\nemoji: \xf0\x9f\x98\x80")), 15000);
        const auto writes = host->writes; emit host->changed(); QTest::qWait(250); QCOMPARE(host->writes, writes);
        QVERIFY(sharing(window, pane, false)); const auto reads = host->reads;
        QTest::keyClick(view, Qt::Key_F7); QTest::qWait(500); QCOMPARE(host->reads, reads); QCOMPARE(host->writes, writes);
        window.disconnectCurrent(); QVERIFY(!pane->clipboardSharingEnabled()); QVERIFY(!pane->clipboardSharingAvailable());
        QPointer<SessionPane> retired = pane;
        QTest::qWait(1000); // The owned xrdp desktop survives transport closure.
        window.reconnectCurrent(); auto* tabs = window.findChild<QTabWidget*>("sessionTabs");
        QTRY_VERIFY(tabs->count() == 1 && tabs->currentWidget() != retired.data());
        pane = qobject_cast<SessionPane*>(tabs->currentWidget()); QVERIFY(pane);
        QVERIFY(acceptCertificate(pane)); QVERIFY(ready(pane)); QVERIFY(!pane->clipboardSharingEnabled());
        QCOMPARE(host->reads, reads); QCOMPARE(host->writes, writes);
        QVERIFY(sharing(window, pane, true)); window.closeTab(0); QCOMPARE(window.sessionCount(), size_t(0));
        const auto closedReads = host->reads; host->copy("synthetic after close"); QCOMPARE(host->reads, closedReads); window.close();
    }
    void savedClipboardSettingStartsSharingThroughShell() {
        // Redirect → Clipboard on a saved connection shares text from the start.
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("confCons.xml"); QVERIFY(createConnection(path, true));
        auto host = std::make_shared<TestClipboardEndpoint>(); host->text = QString::fromUtf8("host-session-clipboard\nemoji: \xf0\x9f\x98\x80");
        MainWindow window(path, false, nullptr, host); QVERIFY(show(window)); auto* pane = connectSaved(window); QVERIFY(pane);
        QVERIFY(pane->target().options.clipboard); QTRY_VERIFY(pane->clipboardSharingEnabled()); QTRY_VERIFY(host->reads > 0);
        auto* view = pane->findChild<DesktopView*>(); QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier, position(view, {400, 400}));
        QTest::qWait(500); QTest::keyClick(view, Qt::Key_F8);
        QTRY_VERIFY_WITH_TIMEOUT(host->text == std::optional<QString>("paste-verified"), 15000);
        window.closeTab(0); QCOMPARE(window.sessionCount(), size_t(0)); window.close();
    }
};
QTEST_MAIN(SessionIntegration)
#include "rdp_session_integration.moc"
