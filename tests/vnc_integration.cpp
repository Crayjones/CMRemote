#include "Appearance.h"
#include "ClipboardDriver.h"
#include "ConnectionXml.h"
#include "GridDriver.h"
#include "MainWindow.h"
#include "SessionPane.h"
#include "VncCredentialsDialog.h"
#include "VncDesktopView.h"
#include "VncWorker.h"
#include <QAction>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSignalSpy>
#include <QTabBar>
#include <QTabWidget>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QUrlQuery>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>

namespace {
const QString profileId = QStringLiteral("be7f9b53-1914-4fe8-bc3c-b51b8e82aa21");
const QColor red("#e03050"), green("#20c070"), blue("#3060e0"), yellow("#e0c030"), cyan("#20c8d8");
QJsonObject manifest;

bool nativePlatform() { return QGuiApplication::platformName() != "offscreen"; }
QString temporaryPath() { return qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/vnc-native-XXXXXX"; }
QByteArray contents(const QString& path) {
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QJsonObject oracle(const QString& route, int client = 0, const QString& action = {}, const QString& password = {}) {
    QUrlQuery query; query.addQueryItem("owner", manifest.value("owner").toString());
    if (client) query.addQueryItem("client", QString::number(client));
    if (!action.isEmpty()) query.addQueryItem("action", action);
    if (!password.isEmpty()) query.addQueryItem("password", password);
    QTcpSocket socket; socket.connectToHost("127.0.0.1", quint16(manifest.value("oracle_port").toInt()));
    if (!socket.waitForConnected(1000)) return {};
    socket.write("GET " + route.toUtf8() + '?' + query.query(QUrl::FullyEncoded).toUtf8() +
                 " HTTP/1.0\r\nHost: owned-vnc-fixture\r\nConnection: close\r\n\r\n");
    if (!socket.waitForBytesWritten(1000)) return {};
    QByteArray response; QElapsedTimer limit; limit.start();
    while (limit.elapsed() < 1500 && response.size() <= 1024 * 1024) {
        response += socket.readAll();
        if (socket.state() == QAbstractSocket::UnconnectedState) break;
        if (!socket.waitForReadyRead(1000)) { response += socket.readAll(); break; }
    }
    if (!response.startsWith("HTTP/1.0 200 ") && !response.startsWith("HTTP/1.1 200 ")) return {};
    const auto end = response.indexOf("\r\n\r\n");
    return end < 0 ? QJsonObject{} : QJsonDocument::fromJson(response.mid(end + 4)).object();
}
QJsonObject clientState(int number) {
    for (const auto& item : oracle("/state").value("clients").toArray())
        if (item.toObject().value("number").toInt() == number) return item.toObject();
    return {};
}
int newestClient() {
    const auto clients = oracle("/state").value("clients").toArray();
    return clients.isEmpty() ? 0 : clients.last().toObject().value("number").toInt();
}
bool control(int client, const QString& action) { return oracle("/control", client, action).value("ok").toBool(); }
bool fixture(const QString& path, bool viewOnly = false) {
    auto xml = ConnectionXml::create(); Secret master(QByteArray("mR3m"));
    SessionTarget target; target.protocol = "vnc"; target.host = "127.0.0.1";
    target.port = quint16(manifest.value("rfb_port").toInt()); target.viewOnly = viewOnly;
    const QJsonObject profile{{"id", profileId}, {"name", "Owned loopback VNC"}, {"kind", "connection"},
        {"parent", QJsonValue::Null}, {"overrides", target.overrides()}};
    const auto projection = QJsonDocument(QJsonObject{{"schema_version", 2}, {"profiles", QJsonArray{profile}}}).toJson(QJsonDocument::Compact);
    if (!xml || !xml->unlock(master) || !xml->patch(master, projection)) return false;
    const auto bytes = xml->serialize(master); if (!bytes) return false;
    QFile output(path); return output.open(QIODevice::WriteOnly) && output.write(*bytes) == bytes->size();
}
bool activate(MainWindow& window, bool initial = false) {
    if (initial) window.resize(1100, 850);
    window.show();
    if (!nativePlatform()) return true;
    if (!QTest::qWaitForWindowExposed(&window, 5000)) return false;
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() == "cocoa") activateNativeTestApplication(&window);
#endif
    window.raise(); window.activateWindow(); window.windowHandle()->requestActivate();
    return QTest::qWaitForWindowActive(&window, 5000);
}
// The fixture offers None by default: like mRemoteNG, the client connects
// without any prompt. /mode?password=1 switches new connections to VncAuth.
bool fixtureRequiresPassword(bool required) {
    return oracle("/mode", 0, {}, required ? "1" : "0").value("ok").toBool();
}
bool noPasswordPrompt(MainWindow& window) { return !window.findChild<VncCredentialsDialog*>(); }
SessionPane* launch(MainWindow& window, bool temporary = false) {
    auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); if (!tabs) return nullptr;
    QPointer<SessionPane> previous = qobject_cast<SessionPane*>(tabs->currentWidget());
    if (!QTest::qWaitFor([&] { return window.documentState() == MainWindow::DocumentState::Open; }, 8000)) return nullptr;
    const int before = oracle("/state").value("accepted").toInt(-1);
    if (temporary) {
        window.setQuickConnectProtocol("vnc"); if (window.quickConnectProtocol() != "vnc") return nullptr;
        window.quickConnect("127.0.0.1:" + QString::number(manifest.value("rfb_port").toInt()));
    } else {
        if (!QTest::qWaitFor([&] { return !window.connectionFiles().isEmpty() &&
            window.selectConnectionProfile({window.connectionFiles().first(), profileId}); }, 8000)) return nullptr;
        auto* connect = window.findChild<QAction*>("actionTreeConnect"); if (!connect || !connect->isEnabled()) return nullptr;
        connect->trigger();
    }
    if (!QTest::qWaitFor([&] { return tabs->currentWidget() && tabs->currentWidget() != previous; }, 8000)) return nullptr;
    if (!QTest::qWaitFor([&] { return oracle("/state").value("accepted").toInt(-2) == before + 1; }, 8000) || !noPasswordPrompt(window)) return nullptr;
    return qobject_cast<SessionPane*>(tabs->currentWidget());
}
bool ready(SessionPane* pane) {
    if (!pane || pane->mock() || pane->target().protocol != "vnc") return false;
    auto* view = pane->findChild<VncDesktopView*>(); auto* status = pane->findChild<QLabel*>("sessionStatus");
    return view && status && QTest::qWaitFor([&] {
        const auto frame = view->frame();
        return pane->active() && status->isHidden() && frame.size() == QSize(640, 360) &&
            frame.pixelColor(16, 16) == red && frame.pixelColor(600, 32) == green &&
            frame.pixelColor(16, 320) == blue && frame.pixelColor(600, 320) == yellow;
    }, 8000);
}
QRect fitRect(DesktopView* view) {
    const QSize size = view->frame().size().scaled(view->size(), Qt::KeepAspectRatio);
    return {QPoint((view->width() - size.width()) / 2, (view->height() - size.height()) / 2), size};
}
QPoint position(DesktopView* view, QPoint remote) {
    const auto fit = fitRect(view); const auto frame = view->frame().size();
    return fit.topLeft() + QPoint(qRound((remote.x() + 0.5) * fit.width() / frame.width()),
                                 qRound((remote.y() + 0.5) * fit.height() / frame.height()));
}
QPoint mapped(DesktopView* view, QPoint local) {
    const auto fit = fitRect(view); const auto frame = view->frame().size();
    return {std::clamp((local.x() - fit.x()) * frame.width() / fit.width(), 0, frame.width() - 1),
            std::clamp((local.y() - fit.y()) * frame.height() / fit.height(), 0, frame.height() - 1)};
}
void move(VncDesktopView* view, QPoint local, Qt::MouseButtons held = Qt::NoButton) {
    QMouseEvent event(QEvent::MouseMove, local, view->mapToGlobal(local), Qt::NoButton, held, Qt::NoModifier);
    QApplication::sendEvent(view, &event);
}
void wheel(VncDesktopView* view, QPoint local, QPoint angle, QPoint pixelDelta = {}, Qt::MouseButtons held = Qt::NoButton) {
    QWheelEvent event(local, view->mapToGlobal(local), pixelDelta, angle, held, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(view, &event);
}
bool near(QColor actual, QColor expected, int tolerance = 8) {
    return qAbs(actual.red() - expected.red()) <= tolerance && qAbs(actual.green() - expected.green()) <= tolerance &&
        qAbs(actual.blue() - expected.blue()) <= tolerance;
}
bool imageSamples(const QImage& image, QSize logical, QPoint origin, DesktopView* view) {
    if (image.isNull() || logical.isEmpty()) return false;
    const QSize frame = view->frame().size();
    const QList<QPoint> samples{{frame.width() / 4, frame.height() / 4}, {3 * frame.width() / 4, frame.height() / 4},
        {frame.width() / 4, 3 * frame.height() / 4}, {3 * frame.width() / 4, 3 * frame.height() / 4}};
    for (const auto remote : samples) {
        const auto point = origin + position(view, remote);
        const QPoint pixel(qRound(qreal(point.x()) * image.width() / logical.width()),
                           qRound(qreal(point.y()) * image.height() / logical.height()));
        if (!image.rect().contains(pixel) || !near(image.pixelColor(pixel), view->frame().pixelColor(remote))) return false;
    }
    const auto fit = fitRect(view);
    QList<QPoint> bars;
    if (fit.top() > 4) bars += QPoint(view->width() / 2, fit.top() / 2);
    if (fit.left() > 4) bars += QPoint(fit.left() / 2, view->height() / 2);
    for (const auto bar : bars) {
        const auto point = origin + bar;
        const QPoint pixel(qRound(qreal(point.x()) * image.width() / logical.width()),
                           qRound(qreal(point.y()) * image.height() / logical.height()));
        if (!image.rect().contains(pixel) || !near(image.pixelColor(pixel), QColor(Qt::black))) return false; // Letterbox bars are black with a frame.
    }
    return true;
}
QJsonArray expectedKeys(std::initializer_list<quint32> symbols) {
    QJsonArray result;
    for (auto symbol : symbols) {
        result.append(QJsonObject{{"sym", qint64(symbol)}, {"down", true}});
        result.append(QJsonObject{{"sym", qint64(symbol)}, {"down", false}});
    }
    return result;
}
bool hasPointer(int client, int mask, QPoint remote) {
    for (const auto& item : clientState(client).value("pointers").toArray()) {
        const auto event = item.toObject();
        if (event.value("mask").toInt(-1) == mask && event.value("x").toInt(-1) == remote.x() && event.value("y").toInt(-1) == remote.y()) return true;
    }
    return false;
}
}

class VncIntegration final : public QObject {
    Q_OBJECT
    bool nativeEvidence_ = false;
    int screenshots_ = 0;

    bool presentation(MainWindow& window, SessionPane* pane, int client, const QString& action = "raw") {
        auto* view = pane->findChild<VncDesktopView*>(); if (!view) return false;
        const quint64 before = view->frameRevision();
        const QColor expected = view->frame().pixelColor(16, 16) == red ? cyan : red;
        const int completed = action == "zrle" ? clientState(client).value("zrle_latencies_ms").toArray().size() : 0;
        QSignalSpy submitted(view, &DesktopView::framePresented), failed(view, &QRhiWidget::renderFailed);
        if (!control(client, action) || !QTest::qWaitFor([&] {
            return view->frameRevision() > before && view->frame().pixelColor(16, 16) == expected;
        }, 8000)) return false;
        if (action == "zrle") {
            // A next request proves UpdateComplete, not just the first changed tile.
            if (!QTest::qWaitFor([&] {
                return clientState(client).value("zrle_latencies_ms").toArray().size() > completed;
            }, 8000) || !QTest::qWaitFor([&] {
                const auto frame = view->frame();
                if (frame.size() != QSize(1920, 1080) || frame.format() != QImage::Format_RGB32) return false;
                const QRgb colors[]{expected.rgb(), green.rgb(), blue.rgb(), yellow.rgb()};
                for (int y = 0; y < frame.height(); ++y) {
                    const auto* row = reinterpret_cast<const QRgb*>(frame.constScanLine(y));
                    for (int x = 0; x < frame.width(); ++x)
                        if (row[x] != colors[(y / 64 * 64 >= 540 ? 2 : 0) + (x >= 960)]) return false;
                }
                return true;
            }, 8000)) return false;
        }
        if (!nativePlatform()) return true; // Intentionally no grabFramebuffer/GPU claim.
        if (!QTest::qWaitFor([&] {
            return view->presentedRevision() >= view->frameRevision() &&
                std::any_of(submitted.cbegin(), submitted.cend(), [before](const auto& frame) {
                    return frame.first().toULongLong() > before;
                });
        }, 8000) || !failed.isEmpty() || !view->rendererAvailable()) return false;
        // A QRhi readback is useful, but cannot by itself establish native window composition.
        const auto gpu = view->grabFramebuffer();
        if (!imageSamples(gpu, view->size(), {}, view)) return false;
        auto* screen = window.windowHandle()->screen(); if (!screen) return false;
        if (!screenCaptureAllowed()) {
            qWarning("Screen Recording access is not granted, so native window capture is skipped. No native QRhi evidence claimed.");
            return false;
        }
        QImage composition; QString method;
        const auto origin = view->mapTo(&window, QPoint());
        const bool captured = QTest::qWaitFor([&] {
            // Capture only the owned client window. The fallback is an OS screen
            // grab cropped to this window's client rect, not QWidget::render/view.frame().
            composition = screen->grabWindow(window.winId()).toImage(); method = "QScreen::grabWindow(owned-window)";
            if (imageSamples(composition, window.size(), origin, view)) return true;
            const auto topLeft = window.mapToGlobal(QPoint());
            composition = screen->grabWindow(0, topLeft.x(), topLeft.y(), window.width(), window.height()).toImage();
            method = "QScreen::grabWindow(owned-client-screen-region)";
            return imageSamples(composition, window.size(), origin, view);
        }, 3000);
        if (!captured) {
            qWarning("Native window capture unavailable or fixture pixels not composed; screen-recording permission/native display may be required. No native QRhi evidence claimed.");
            return false;
        }
        const auto prefix = manifest.value("evidence_dir").toString() + '/' +
            QString::fromLatin1(QTest::currentTestFunction()) + '-' + QString::number(++screenshots_);
        if (!gpu.save(prefix + "-qrhi.png") || !composition.save(prefix + "-window.png")) return false;
        qInfo("Owned VNC composition verified with %s; revision=%llu; window capture=%s", qPrintable(method),
              static_cast<unsigned long long>(view->presentedRevision()), qPrintable(prefix + "-window.png"));
        nativeEvidence_ = true; return true;
    }
    bool focus(MainWindow& window, VncDesktopView* view) {
        if (!activate(window)) return false;
        view->setFocus(); return QTest::qWaitFor([&] { return view->hasFocus(); }, 5000);
    }
    bool reconnect(MainWindow& window, QPointer<SessionPane> old) {
        window.reconnectCurrent();
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs");
        return QTest::qWaitFor([&] { return tabs->currentWidget() && tabs->currentWidget() != old.data(); }, 8000) &&
            ready(qobject_cast<SessionPane*>(tabs->currentWidget())) && noPasswordPrompt(window);
    }
private slots:
    void initTestCase() {
        applyLightAppearance(*qApp); qApp->setQuitOnLastWindowClosed(false);
        // The runner's manifest is opt-in and owner-scoped. Cocoa already forwards
        // XDG_CONFIG_HOME; no test port is smuggled through unrelated credential env vars.
        const auto build = QDir(qEnvironmentVariable("RM_BUILD_DIR")).canonicalPath();
        const auto config = QDir(qEnvironmentVariable("XDG_CONFIG_HOME")).canonicalPath();
        QVERIFY2(!build.isEmpty() && config.startsWith(build + "/test-data/vnc-"), "Run this opt-in test only through scripts/test-vnc.py");
        manifest = QJsonDocument::fromJson(contents(config + "/vnc-fixture.json")).object();
        QVERIFY(manifest.value("schema").toInt() == 1 && manifest.value("opt_in").toBool());
        QCOMPARE(manifest.value("host").toString(), QString("127.0.0.1"));
        QCOMPARE(manifest.value("platform").toString(), QGuiApplication::platformName());
        QVERIFY(manifest.value("owner").toString().size() == 32);
        for (const auto* key : {"rfb_port", "oracle_port"}) QVERIFY(manifest.value(key).toInt() > 0 && manifest.value(key).toInt() <= 65535);
        const auto evidence = QDir(manifest.value("evidence_dir").toString()).canonicalPath();
        QVERIFY(!evidence.isEmpty() && evidence.startsWith(build + "/test-data/vnc-"));
        QCOMPARE(oracle("/state").value("owner").toString(), manifest.value("owner").toString());
        QVERIFY(QDir().mkpath(build + "/test-data"));
        // Each runner invocation selects one case. Bound a failed native launch,
        // including asynchronous worker retirement, to less than 150 seconds.
        QTimer::singleShot(145000, this, [] { qFatal("Owned VNC acceptance exceeded 145 seconds"); });
        if (!nativePlatform()) qInfo("Offscreen VNC is CPU/transport/input only; native QRhi composition is not evidence.");
    }
    void cleanup() {
        QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(oracle("/state").value("active").toInt(-1), 0, 5000);
        QVERIFY2(oracle("/state").value("errors").toArray().isEmpty(), "Owned fixture observed an unexpected protocol message");
    }
    void cleanupTestCase() {
        if (nativePlatform()) QVERIFY2(nativeEvidence_, "Cannot claim native QRhi evidence without independent window composition capture");
        qInfo("RM_VNC_NATIVE_COMPOSITION=%d", nativePlatform() && nativeEvidence_ ? 1 : 0);
    }
    void savedVncRenderingThroughShell() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid());
        const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path)); const auto original = contents(path);
        auto clipboard = std::make_shared<TestClipboardEndpoint>();
        MainWindow window(path, true, nullptr, clipboard, directory.filePath("trust.json"), directory.filePath("appearance.json"), directory.filePath("preferences.json"));
        QVERIFY(activate(window, true)); auto* pane = launch(window); QVERIFY(pane); QVERIFY(ready(pane));
        QCOMPARE(pane->profile(), profileId); QVERIFY(pane->document().valid()); QCOMPARE(window.sessionCount(), size_t(1));
        auto* view = pane->findChild<VncDesktopView*>(); const int client = newestClient(); QVERIFY(client > 0);
        QTRY_COMPARE(view->geometry(), pane->contentsRect()); QVERIFY(!pane->clipboardSharingAvailable());
        QVERIFY(presentation(window, pane, client));
        QVERIFY(control(client, "copy"));
        QTRY_COMPARE_WITH_TIMEOUT(view->frame().pixelColor(496, 264), cyan, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(clientState(client).value("copy_updates").toInt(), 1, 5000);
        move(view, position(view, {120, 90}));
        QTRY_COMPARE_WITH_TIMEOUT(view->cursor().shape(), Qt::BitmapCursor, 5000);
        const auto cursor = view->cursor(); const auto cursorImage = cursor.pixmap().toImage(); QVERIFY(!cursorImage.isNull());
        const QSize logical = cursorImage.size() / cursorImage.devicePixelRatio();
        const QSize fitted = fitRect(view).size();
        const qreal scaleX = qreal(fitted.width()) / view->frame().width(), scaleY = qreal(fitted.height()) / view->frame().height();
        QVERIFY(qAbs(logical.width() - qRound(16 * scaleX)) <= 1); QVERIFY(qAbs(logical.height() - qRound(16 * scaleY)) <= 1);
        QCOMPARE(cursor.hotSpot(), QPoint(qRound(3 * scaleX), qRound(5 * scaleY)));
        QVERIFY(cursorImage.pixelColor(cursorImage.width() / 4, cursorImage.height() / 2).alpha() > 200);
        QVERIFY(cursorImage.pixelColor(3 * cursorImage.width() / 4, cursorImage.height() / 2).alpha() < 10);
        QVERIFY(control(client, "hide-cursor")); QTRY_COMPARE_WITH_TIMEOUT(view->cursor().shape(), Qt::BlankCursor, 5000);
        QVERIFY(control(client, "cursor")); QTRY_COMPARE_WITH_TIMEOUT(view->cursor().shape(), Qt::BitmapCursor, 5000);
        const auto revision = view->frameRevision(); const int empty = clientState(client).value("empty_updates").toInt();
        QTRY_VERIFY_WITH_TIMEOUT(clientState(client).value("empty_updates").toInt() >= empty + 3, 5000);
        QCOMPARE(view->frameRevision(), revision); QVERIFY(pane->active()); QVERIFY(pane->findChild<QLabel*>("sessionStatus")->isHidden());
        const auto session = pane->id(); const auto target = pane->target();
        window.resize(900, 680); QTest::qWait(150); QCOMPARE(view->frame().size(), QSize(640, 360));
        QVERIFY(presentation(window, pane, client));
        QVERIFY(control(client, "resize"));
        QTRY_COMPARE_WITH_TIMEOUT(view->frame().size(), QSize(800, 480), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(view->frame().pixelColor(720, 400), yellow, 5000);
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            const auto requests = clientState(client).value("request_sizes").toArray();
            return !requests.isEmpty() && requests.last().toObject().value("width").toInt() == 800 &&
                requests.last().toObject().value("height").toInt() == 480;
        }(), 5000);
        QVERIFY(presentation(window, pane, client));
        QCOMPARE(clientState(client).value("resize_updates").toInt(), 1);
        QCOMPARE(clientState(client).value("max_outstanding").toInt(), 1);
        QCOMPARE(pane->id(), session); QVERIFY(pane->target() == target); QCOMPARE(contents(path), original);
        QCOMPARE(clipboard->reads, 0); QCOMPARE(clipboard->writes, 0); QCOMPARE(clipboard->watches, 0);
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 8000);
    }
    void vncZrleTileBurstThroughShell() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid());
        const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path));
        MainWindow window(path, true, nullptr, std::make_shared<TestClipboardEndpoint>(), directory.filePath("trust.json"), directory.filePath("appearance.json"), directory.filePath("preferences.json"));
        QVERIFY(activate(window, true)); auto* pane = launch(window); QVERIFY(pane); QVERIFY(ready(pane));
        const int client = newestClient(); QVERIFY(client > 0);
        // Two updates exercise continued zlib history and removal of stale colors.
        QVERIFY(presentation(window, pane, client, "zrle"));
        QVERIFY(presentation(window, pane, client, "zrle"));
        const auto state = clientState(client); const auto latencies = state.value("zrle_latencies_ms").toArray();
        QCOMPARE(state.value("zrle_updates").toInt(), 2); QCOMPARE(latencies.size(), 2);
        QCOMPARE(state.value("max_outstanding").toInt(), 1);
        for (const auto elapsed : latencies) {
            qInfo("Production 510-tile ZRLE update through next request: %.3f ms", elapsed.toDouble());
            QVERIFY2(elapsed.toDouble() < 500, "Production tile delivery incurred polling delays");
        }
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 8000);
    }
    void temporaryVncInputThroughShell() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid());
        auto clipboard = std::make_shared<TestClipboardEndpoint>();
        MainWindow window(directory.filePath("connections.xml"), true, nullptr, clipboard, directory.filePath("trust.json"), directory.filePath("appearance.json"), directory.filePath("preferences.json"));
        QVERIFY(activate(window, true)); auto* pane = launch(window, true); QVERIFY(pane); QVERIFY(ready(pane));
        QVERIFY(pane->profile().isEmpty()); QVERIFY(!pane->document().valid());
        auto* view = pane->findChild<VncDesktopView*>(); const int client = newestClient();
        QVERIFY(presentation(window, pane, client)); QVERIFY(focus(window, view)); QVERIFY(control(client, "clear-input"));
        QTest::keyClick(view, Qt::Key_A); QTest::keyClick(view, Qt::Key_Tab); QTest::keyClick(view, Qt::Key_Left);
        QTest::keyClick(view, Qt::Key_F11); // Remains remote, does not toggle local fullscreen.
        QInputMethodEvent preedit(QString::fromUtf8("界"), {}); QApplication::sendEvent(view, &preedit);
        QTest::keyClick(view, Qt::Key_X); // Composition keystroke must not leak to RFB.
        QInputMethodEvent unicode; unicode.setCommitString(QString::fromUtf8("é界😀é")); QApplication::sendEvent(view, &unicode);
        const auto expected = expectedKeys({0x61, 0xff09, 0xff51, 0xffc8, 0xe9, 0x0100754c, 0x0101f600, 0x65, 0x01000301});
        QTRY_COMPARE_WITH_TIMEOUT(clientState(client).value("keys").toArray(), expected, 5000);
        QVERIFY(!window.isFullScreen()); QVERIFY(view->hasFocus());
        QVERIFY(control(client, "clear-input"));
#ifdef Q_OS_MACOS
        const auto physical = QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta) ? Qt::Key_Control : Qt::Key_Meta;
#else
        const auto physical = Qt::Key_Control;
#endif
        QTest::keyPress(view, physical); QTest::keyRelease(view, physical);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier, "b"); QApplication::sendEvent(view, &press);
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier, "b", true); QApplication::sendEvent(view, &repeat);
        QKeyEvent repeatRelease(QEvent::KeyRelease, Qt::Key_B, Qt::NoModifier, "b", true); QApplication::sendEvent(view, &repeatRelease);
        auto* quick = window.findChild<QComboBox*>("quickConnectCombo"); QVERIFY(quick && quick->lineEdit());
        quick->lineEdit()->setFocus(); QTRY_VERIFY(view->hasFocus() == false);
        auto expectedHeld = expectedKeys({0xffe3});
        expectedHeld.append(QJsonObject{{"sym", 0x62}, {"down", true}});
        expectedHeld.append(QJsonObject{{"sym", 0x62}, {"down", true}});
        expectedHeld.append(QJsonObject{{"sym", 0x62}, {"down", false}});
        QTRY_COMPARE_WITH_TIMEOUT(clientState(client).value("keys").toArray(), expectedHeld, 5000);
        QVERIFY(focus(window, view)); QVERIFY(control(client, "clear-input"));
        const auto point = position(view, {160, 100}); const auto remote = mapped(view, point);
        QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mousePress(view, Qt::RightButton, Qt::NoModifier, point);
        move(view, view->rect().bottomRight() + QPoint(50, 50), Qt::LeftButton | Qt::RightButton);
        QTest::mouseRelease(view, Qt::RightButton, Qt::NoModifier, view->rect().bottomRight() + QPoint(50, 50));
        QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, view->rect().bottomRight() + QPoint(50, 50));
        QTRY_VERIFY_WITH_TIMEOUT(hasPointer(client, 1, remote), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(hasPointer(client, 5, remote), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(hasPointer(client, 5, {639, 359}), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(hasPointer(client, 1, {639, 359}), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(hasPointer(client, 0, {639, 359}), 5000);
        QVERIFY(control(client, "clear-input"));
        QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, point);
        wheel(view, point, {120, 120}, {}, Qt::LeftButton);
        wheel(view, point, {-120, -120}, {}, Qt::LeftButton);
        // Trackpad accumulation must not drop a held button, or duplicate angle+pixel input.
        wheel(view, point, {}, {0, 20}, Qt::LeftButton); wheel(view, point, {}, {0, 20}, Qt::LeftButton);
        QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, point);
        QTRY_VERIFY_WITH_TIMEOUT(hasPointer(client, 65, remote) && hasPointer(client, 9, remote) &&
            hasPointer(client, 33, remote) && hasPointer(client, 17, remote) && hasPointer(client, 0, remote), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(clientState(client).value("pointers").toArray().size(), 12, 5000);
        const auto fit = fitRect(view); QVERIFY(fit.top() > 0 || fit.left() > 0);
        QVERIFY(control(client, "clear-input"));
        const QPoint bar = fit.top() ? QPoint(view->width() / 2, 0) : QPoint(0, view->height() / 2);
        QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier, bar); wheel(view, bar, {0, 120});
        const int empty = clientState(client).value("empty_updates").toInt();
        QTRY_VERIFY_WITH_TIMEOUT(clientState(client).value("empty_updates").toInt() > empty, 5000);
        QVERIFY(clientState(client).value("pointers").toArray().isEmpty());
        QCOMPARE(clipboard->reads, 0); QCOMPARE(clipboard->writes, 0); QCOMPARE(clipboard->watches, 0);
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 8000);
    }
    void vncFullscreenGeometryThroughShell() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid());
        const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path)); const auto original = contents(path);
        MainWindow window(path, true, nullptr, std::make_shared<TestClipboardEndpoint>(), directory.filePath("trust.json"), directory.filePath("appearance.json"), directory.filePath("preferences.json"));
        QVERIFY(activate(window, true)); auto* pane = launch(window); QVERIFY(pane); QVERIFY(ready(pane));
        auto* view = pane->findChild<VncDesktopView*>(); const int client = newestClient(); QVERIFY(presentation(window, pane, client));
        auto* connections = window.findChild<QDockWidget*>("connectionsDock"); auto* config = window.findChild<QDockWidget*>("configDock");
        auto* sessionFull = window.findChild<QAction*>("actionTabFullscreen"); auto* appFull = window.findChild<QAction*>("actionFullscreen");
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); QVERIFY(connections && config && sessionFull && appFull && tabs);
        config->hide(); QTest::qWait(100);
        const auto geometry = window.geometry(); auto* const viewParent = view->parentWidget(); auto* const paneParent = pane->parentWidget();
        const auto session = pane->id(); const auto target = pane->target(); const auto margins = pane->layout()->contentsMargins();
        const int authenticated = oracle("/state").value("authenticated").toInt();
        // Session fullscreen opens its own window; the main window keeps its layout.
        QVERIFY(sessionFull->isEnabled()); sessionFull->trigger(); QPointer<QMainWindow> full;
        QTRY_VERIFY_WITH_TIMEOUT((full = window.sessionFullscreenWindow()) && full->isFullScreen(), 8000);
        QVERIFY(!window.isFullScreen()); QVERIFY(connections->isVisible()); QVERIFY(config->isHidden()); QVERIFY(full->isAncestorOf(pane)); QCOMPARE(tabs->count(), 0);
        auto* row = full->findChild<QWidget*>("fullscreenControlsRow"); QVERIFY(row && row->parentWidget() == full->centralWidget());
        QVERIFY(!pane->findChild<QWidget*>("fullscreenBar")); QCOMPARE(view->parentWidget(), viewParent);
        QCOMPARE(pane->layout()->contentsMargins(), margins); QCOMPARE(view->frame().size(), QSize(640, 360));
        QVERIFY(presentation(window, pane, client));
        auto* exit = full->findChild<QToolButton*>("exitSessionFullscreen"); QVERIFY(exit); exit->click();
        QTRY_VERIFY_WITH_TIMEOUT(!window.sessionFullscreenWindow(), 8000); QCOMPARE(tabs->indexOf(pane), 0); QCOMPARE(pane->parentWidget(), paneParent);
        QCOMPARE(window.geometry(), geometry); QVERIFY(connections->isVisible()); QVERIFY(config->isHidden()); QVERIFY(tabs->tabBar()->isVisible());
        QVERIFY(presentation(window, pane, client));
        // Application fullscreen is independent of session fullscreen.
        appFull->setChecked(true); QTRY_VERIFY_WITH_TIMEOUT(window.isFullScreen(), 8000); QVERIFY(connections->isVisible());
        sessionFull->trigger(); QTRY_VERIFY_WITH_TIMEOUT((full = window.sessionFullscreenWindow()) && full->isFullScreen(), 8000); QVERIFY(connections->isVisible());
        exit = full->findChild<QToolButton*>("exitSessionFullscreen"); QVERIFY(exit); exit->click();
        QTRY_VERIFY_WITH_TIMEOUT(!window.sessionFullscreenWindow() && window.isFullScreen() && connections->isVisible(), 8000);
        appFull->setChecked(false); QTRY_VERIFY_WITH_TIMEOUT(!window.isFullScreen(), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(window.geometry(), geometry, 5000);
        QVERIFY(presentation(window, pane, client));
        QCOMPARE(pane->id(), session); QVERIFY(pane->target() == target); QCOMPARE(view->parentWidget(), viewParent);
        QCOMPARE(oracle("/state").value("authenticated").toInt(), authenticated); QCOMPARE(contents(path), original);
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 8000);
    }
    void vncDisconnectReconnectThroughShell() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid());
        const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path)); const auto original = contents(path);
        MainWindow window(path, true, nullptr, std::make_shared<TestClipboardEndpoint>(), directory.filePath("trust.json"), directory.filePath("appearance.json"), directory.filePath("preferences.json"));
        QVERIFY(activate(window, true)); QPointer<SessionPane> saved = launch(window); QVERIFY(saved); QVERIFY(ready(saved));
        const auto target = saved->target(); const auto originalSession = saved->id(); int client = newestClient();
        QVERIFY(presentation(window, saved, client)); auto* view = saved->findChild<VncDesktopView*>();
        move(view, position(view, {120, 90})); QTRY_COMPARE_WITH_TIMEOUT(view->cursor().shape(), Qt::BitmapCursor, 5000);
        window.disconnectCurrent(); QCOMPARE(window.sessionCount(), size_t(1)); QVERIFY(!saved->active());
        QCOMPARE(view->cursor().shape(), Qt::ArrowCursor); QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 8000);
        const int before = oracle("/state").value("authenticated").toInt();
        QVERIFY(reconnect(window, saved));
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); QPointer<SessionPane> replacement = qobject_cast<SessionPane*>(tabs->currentWidget());
        QVERIFY(replacement); QVERIFY(replacement->id() != originalSession); QVERIFY(replacement->target() == target);
        QTRY_VERIFY(saved.isNull()); QTRY_COMPARE_WITH_TIMEOUT(oracle("/state").value("authenticated").toInt(), before + 1, 5000);
        client = newestClient(); QVERIFY(presentation(window, replacement, client));
        QVERIFY(control(client, "disconnect"));
        QTRY_VERIFY_WITH_TIMEOUT(!replacement->findChild<QLabel*>("sessionStatus")->isHidden(), 8000);
        QCOMPARE(replacement->findChild<VncDesktopView*>()->cursor().shape(), Qt::ArrowCursor);
        QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 8000);
        QVERIFY(reconnect(window, replacement)); window.closeTab(0); QTRY_COMPARE(window.sessionCount(), size_t(0));
        QPointer<SessionPane> temporary = launch(window, true); QVERIFY(temporary); QVERIFY(ready(temporary));
        const auto temporarySession = temporary->id(); window.disconnectCurrent();
        QVERIFY(reconnect(window, temporary)); auto* newTemporary = qobject_cast<SessionPane*>(tabs->currentWidget()); QVERIFY(newTemporary);
        QVERIFY(newTemporary->profile().isEmpty()); QVERIFY(newTemporary->id() != temporarySession); QVERIFY(!newTemporary->document().valid());
        QCOMPARE(contents(path), original); window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 8000);
    }
    void vncPasswordPromptThroughShell() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid());
        const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path));
        MainWindow window(path, true, nullptr, std::make_shared<TestClipboardEndpoint>(), directory.filePath("trust.json"), directory.filePath("appearance.json"), directory.filePath("preferences.json"));
        QVERIFY(activate(window, true)); QVERIFY(fixtureRequiresPassword(true));
        QVERIFY(QTest::qWaitFor([&] { return window.documentState() == MainWindow::DocumentState::Open &&
            window.selectConnectionProfile({window.connectionFiles().first(), profileId}); }, 8000));
        window.findChild<QAction*>("actionTreeConnect")->trigger();
        // The popup appears only once the server asks for a password.
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<VncCredentialsDialog*>(), 8000);
        auto* dialog = window.findChild<VncCredentialsDialog*>(); QCOMPARE(dialog->windowTitle(), QString("Password"));
        const int client = newestClient(); QVERIFY(clientState(client).value("password_requested").toBool());
        auto* input = dialog->findChild<QLineEdit*>("passwordEdit"); QVERIFY(input);
        if (nativePlatform()) {
            const auto evidence = manifest.value("evidence_dir").toString();
            QVERIFY(QTest::qWaitForWindowExposed(dialog));
            auto* screen = dialog->windowHandle()->screen(); const auto frame = dialog->frameGeometry();
            QCOMPARE(dialog->windowModality(), Qt::ApplicationModal); QVERIFY(dialog->frameGeometry().height() > dialog->geometry().height());
            QTest::qWait(300);
            if (screenCaptureAllowed()) QVERIFY(screen->grabWindow(0, frame.x(), frame.y(), frame.width(), frame.height()).save(evidence + "/password-dialog.png"));
            QVERIFY(window.findChild<QDockWidget*>("configDock")->grab().save(evidence + "/vnc-config.png"));
        }
        if (nativePlatform()) { QTRY_VERIFY_WITH_TIMEOUT(input->hasFocus(), 5000); QTest::keyClicks(input, "password"); }
        else input->setText("password");
        QTest::keyClick(input, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!window.findChild<VncCredentialsDialog*>(), 5000);
        auto* pane = qobject_cast<SessionPane*>(window.findChild<QTabWidget*>("sessionTabs")->currentWidget()); QVERIFY(ready(pane));
        QVERIFY(clientState(client).value("password_accepted").toBool()); QVERIFY(presentation(window, pane, client));
        QVERIFY(fixtureRequiresPassword(false)); window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 8000);
    }
    void vncResponsiveCloseThroughShell() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid());
        const auto path = directory.filePath("connections.xml"); QVERIFY(fixture(path));
        auto clipboard = std::make_shared<TestClipboardEndpoint>();
        MainWindow window(path, true, nullptr, clipboard, directory.filePath("trust.json"), directory.filePath("appearance.json"), directory.filePath("preferences.json"));
        QVERIFY(activate(window, true)); auto* first = launch(window); QVERIFY(first); QVERIFY(ready(first)); const int firstClient = newestClient();
        QVERIFY(presentation(window, first, firstClient));
        auto* second = launch(window, true); QVERIFY(second); QVERIFY(ready(second)); const int secondClient = newestClient();
        QVERIFY(firstClient != secondClient); QCOMPARE(window.sessionCount(), size_t(2));
        QVERIFY(control(firstClient, "hold") && control(secondClient, "hold"));
        const auto emptyFirst = clientState(firstClient).value("empty_updates").toInt();
        const auto emptySecond = clientState(secondClient).value("empty_updates").toInt();
        QTest::qWait(150); // Both workers now wait on a deliberately unanswered update.
        QCOMPARE(clientState(firstClient).value("empty_updates").toInt(), emptyFirst);
        QCOMPARE(clientState(secondClient).value("empty_updates").toInt(), emptySecond);
        int beats = 0; QTimer heartbeat; connect(&heartbeat, &QTimer::timeout, this, [&] { ++beats; }); heartbeat.start(1);
        QElapsedTimer elapsed; elapsed.start(); window.close();
        QVERIFY2(elapsed.elapsed() < 100, "GUI close synchronously waited for the native VNC worker");
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 8000); QVERIFY(beats > 0);
        QCOMPARE(window.sessionCount(), size_t(0)); QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(oracle("/state").value("active").toInt(-1), 0, 5000);
        QCOMPARE(clipboard->reads, 0); QCOMPARE(clipboard->writes, 0); QCOMPARE(clipboard->watches, 0);
    }
};
QTEST_MAIN(VncIntegration)
#include "vnc_integration.moc"
