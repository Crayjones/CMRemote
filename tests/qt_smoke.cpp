#include "MainWindow.h"
#include "ThemeIcons.h"
#include "UpdateCheck.h"
#include "QuickConnect.h"
#include "AutoUpdate.h"
#include <QTextDocument>
#include "Appearance.h"
#include "Branding.h"
#include "SessionPane.h"
#include "SessionFullscreenController.h"
#include "Views.h"
#include "VncDesktopView.h"
#include "Credentials.h"
#include "RdpTarget.h"
#include "DataPaths.h"
#include "DocumentService.h"
#include "GridDriver.h"
#include "ClipboardDriver.h"
#include "PanelWindow.h"
#include <QApplication>
#include <QDialog>
#include <QDockWidget>
#include <QMenuBar>
#include <QAction>
#include <QTimer>
#ifdef Q_OS_MACOS
QStringList nativeMenuTitles(const QString& parent = {});
QStringList nativeApplicationMenuTitles();
bool triggerNativeMenuItem(const QString& parent, const QString& title);
bool triggerNativePreferencesShortcut();
#endif
#ifdef RM_WITH_RDP
#include "RdpWorker.h"
#include <freerdp/freerdp.h>
#include <freerdp/settings.h>
#endif
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QSignalSpy>
#include <QKeyEvent>
#include <QSpinBox>
#include <QTabWidget>
#include <QTabBar>
#include <QMenu>
#include <QContextMenuEvent>
#include <QInputDialog>
#include <QListWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QComboBox>
#include <QToolBar>
#include <QToolButton>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QScreen>
#include <QWindow>
#include <QVBoxLayout>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#define WAIT_STORE(window) do { QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open); QVERIFY(waitGridSaved(window)); } while (false)

#define REQUIRE_RHI() do { if (QGuiApplication::platformName() == "offscreen" || qEnvironmentVariable("QT_WIDGETS_RHI_BACKEND") == "null") QSKIP("Requires real window-system rendering"); QTest::failOnWarning("QRhiWidget: No QRhi"); QTest::failOnWarning(QRegularExpression("QRhi .*going down with.*unreleased resources")); } while (false)

class TestDesktop : public DesktopView {
public:
    using DesktopView::DesktopView;
    using DesktopView::rhi;
    QByteArray failStage;
    int renders = 0, targetSamples = 0;
    bool msaaWithoutColorTexture = false;
    QList<int> supportedSamples;
    std::function<void()> afterRender;
    void dropResources() { releaseResources(); update(); }
protected:
    bool allowResourceCreation(const char* stage) const override { return failStage != stage; }
    void render(QRhiCommandBuffer* cb) override {
        ++renders; targetSamples = renderTarget()->sampleCount();
        msaaWithoutColorTexture = targetSamples > 1 && !colorTexture();
        supportedSamples = rhi()->supportedSampleCounts();
        DesktopView::render(cb);
        if (afterRender) { auto action = std::move(afterRender); afterRender = {}; action(); }
    }
};
static QImage solidFrame(QColor color, QSize size = {200, 100}) {
    QImage image(size, QImage::Format_RGB32); image.fill(color); return image;
}
static bool revisionSubmitted(const QSignalSpy& spy, quint64 revision) {
    return std::any_of(spy.cbegin(), spy.cend(), [revision](const auto& entry) { return entry.at(0).toULongLong() == revision; });
}
static QColor capturedCenter(DesktopView& view) {
    const auto shot = view.grabFramebuffer();
    return shot.isNull() ? QColor() : shot.pixelColor(shot.width() / 2, shot.height() / 2);
}

// The session's own fullscreen window once it has entered fullscreen.
static QMainWindow* fullscreenWindow(const MainWindow& w) {
    auto* window = w.sessionFullscreenWindow(); return window && window->isFullScreen() ? window : nullptr;
}
static void tabMenuAction(MainWindow& w, QTabWidget* tabs, int index, const char* name) {
    QVERIFY(tabs);
    const auto center = tabs->tabBar()->tabRect(index).center();
    QContextMenuEvent event(QContextMenuEvent::Mouse, center, tabs->tabBar()->mapToGlobal(center));
    QApplication::sendEvent(tabs->tabBar(), &event);
    const auto menus = w.findChildren<QMenu*>("sessionContextMenu"); QVERIFY(!menus.isEmpty());
    auto* action = menus.last()->findChild<QAction*>(name); QVERIFY(action && action->isEnabled());
    menus.last()->close(); action->trigger();
}
static QList<PanelWindow*> panelWindows() {
    QList<PanelWindow*> result;
    for (auto* widget : QApplication::topLevelWidgets()) if (auto* window = qobject_cast<PanelWindow*>(widget); window && window->isVisible()) result << window;
    return result;
}
// Presses a tab, then moves through the given global points and releases at the last.
static bool dragTab(QTabBar* bar, int index, const QList<QPoint>& path, const std::function<void()>& beforeDrop = {}) {
    const auto start = bar->tabRect(index).center(); if (start.x() < 0) return false;
    QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, start);
    for (const auto& global : path) {
        QMouseEvent move(QEvent::MouseMove, QPointF(bar->mapFromGlobal(global)), QPointF(global), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(bar, &move);
    }
    if (beforeDrop) beforeDrop();
    const auto end = path.last();
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(bar->mapFromGlobal(end)), QPointF(end), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(bar, &release); return release.isAccepted() && !panelWindows().isEmpty();
}
static bool nearlyEqual(const QImage& left, const QImage& right, int tolerance) {
    const auto a = left.convertToFormat(QImage::Format_ARGB32_Premultiplied), b = right.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (a.size() != b.size()) return false;
    for (int y = 0; y < a.height(); ++y) {
        const auto* p = reinterpret_cast<const QRgb*>(a.constScanLine(y)); const auto* q = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x)
            if (qAbs(qAlpha(p[x]) - qAlpha(q[x])) > tolerance || qAbs(qRed(p[x]) - qRed(q[x])) > tolerance ||
                qAbs(qGreen(p[x]) - qGreen(q[x])) > tolerance || qAbs(qBlue(p[x]) - qBlue(q[x])) > tolerance) return false;
    }
    return true;
}
// Cocoa's icon engine can draw a size from the next larger asset.
static bool brandingAsset(const QImage& actual, int size) {
    const QImage expected(QString(":/branding/cmremote-%1.png").arg(size)), larger(QString(":/branding/cmremote-%1.png").arg(size * 2));
    return nearlyEqual(actual, expected, 2) ||
           (!larger.isNull() && nearlyEqual(actual, larger.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), 2));
}
class Smoke : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qputenv("QT_RHI_LEAK_CHECK", "1"); applyLightAppearance(*qApp); qApp->setQuitOnLastWindowClosed(false); }
    void firstPartyBrandingUsesSourcePixelsAndPreservesIdentity() {
        const auto applicationName = QCoreApplication::applicationName();
        const auto organizationName = QCoreApplication::organizationName();
        QTemporaryDir dir; QVERIFY(dir.isValid());
        MainWindow window(dir.filePath("confCons.xml"), true); WAIT_STORE(window);
        PanelWindow detached(2);
        QCOMPARE(QCoreApplication::applicationName(), applicationName);
        QCOMPARE(QCoreApplication::organizationName(), organizationName);
        QCOMPARE(QGuiApplication::applicationDisplayName(), QString("CMRemote"));
        QCOMPARE(QGuiApplication::desktopFileName(), QString("org.remotemanager.RemoteManager"));
        QCOMPARE(window.windowTitle(), QString("CMRemote"));
        QCOMPARE(detached.windowTitle(), QString("CMRemote — Window 2"));
        auto* groups = window.findChild<QTabWidget*>("groupTabs"); QVERIFY(groups);
        const int general = groups->indexOf(window.findChild<QWidget*>("generalPanel")); QVERIFY(general >= 0);
        const QList<QIcon> icons{Branding::applicationIcon(), QGuiApplication::windowIcon(), window.windowIcon(),
                                 groups->tabIcon(general), detached.windowIcon()};
        for (const auto& icon : icons) {
            QCOMPARE(icon.availableSizes().size(), 9);
            for (const int size : {16, 24, 32, 48, 64, 128, 256, 512, 1024}) {
                const QImage expected(QString(":/branding/cmremote-%1.png").arg(size));
                QVERIFY(!expected.isNull()); QCOMPARE(expected.size(), QSize(size, size));
                QVERIFY(icon.availableSizes().contains(expected.size()));
                const auto actual = icon.pixmap(expected.size(), 1.0).toImage();
                QVERIFY(brandingAsset(actual, size));
            }
        }
        auto* options = detached.findChild<QToolButton*>("panelWindowMenuButton"); QVERIFY(options);
        QVERIFY(nearlyEqual(options->icon().pixmap(QSize(16, 16), 1.0).toImage(), QIcon(":/original/Panel_16x.png").pixmap(QSize(16, 16), 1.0).toImage(), 0));
        auto* aboutAction = window.findChild<QAction*>("actionAbout"); QVERIFY(aboutAction);
        QCOMPARE(aboutAction->text(), QString("About CMRemote")); aboutAction->trigger();
        auto* about = window.findChild<QMessageBox*>("aboutDialog"); QVERIFY(about);
#ifndef Q_OS_MACOS
        // QMessageBox ignores window titles on macOS, including offscreen.
        QCOMPARE(about->windowTitle(), QString("About CMRemote"));
#endif
        QCOMPARE(about->textFormat(), Qt::RichText);
        auto* aboutLabel = about->findChild<QLabel*>("qt_msgbox_label"); QVERIFY(aboutLabel && aboutLabel->openExternalLinks());
        QTextDocument aboutText; aboutText.setHtml(about->text());
        QCOMPARE(aboutText.toPlainText(), QString("CMRemote — Cross-platform Multi Remote\nhttps://github.com/Crayjones/CMRemote"));
        QVERIFY(Updates::compareVersions(Updates::currentVersion(), "0.0.1").has_value());
        QVERIFY(about->text().contains("href=\"https://github.com/Crayjones/CMRemote\""));
        const auto aboutImage = about->iconPixmap().toImage(); QVERIFY(!aboutImage.isNull());
        const QImage aboutExpected(QString(":/branding/cmremote-%1.png").arg(aboutImage.width()));
        QVERIFY(!aboutExpected.isNull());
        QVERIFY(brandingAsset(aboutImage, aboutImage.width()));
        about->accept();
    }
#ifdef Q_OS_MACOS
    void nativeActivationIgnoresOffscreenHandle() {
        if (QGuiApplication::platformName() != "offscreen") QSKIP("Offscreen activation regression only");
        QWidget window; window.show();
        activateNativeTestApplication(&window);
        QVERIFY(window.isVisible());
    }
#endif
    void nativeWindowRetainsActivationWithoutTransport_data() {
        QTest::addColumn<bool>("withRhi");
        QTest::newRow("raster") << false; QTest::newRow("rhi") << true;
    }
    void nativeWindowRetainsActivationWithoutTransport() {
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() != "cocoa") QSKIP("AppKit requires Cocoa");
        QFETCH(bool, withRhi);
        QWidget window; QVBoxLayout layout(&window); layout.setContentsMargins(0, 0, 0, 0);
        std::unique_ptr<TestDesktop> view;
        if (withRhi) { view = std::make_unique<TestDesktop>(false); layout.addWidget(view.get()); }
        window.resize(512, 384); window.show();
        activateNativeTestApplication(&window);
        QVERIFY(QTest::qWaitForWindowExposed(&window)); QVERIFY(QTest::qWaitForWindowActive(&window));
        if (view) {
            QSignalSpy submitted(view.get(), &DesktopView::framePresented);
            view->setFrame(solidFrame(Qt::darkCyan, {1024, 768}));
            QTRY_VERIFY(revisionSubmitted(submitted, view->frameRevision()));
        }
        QTest::qWait(3500);
        qInfo("RM_IDLE_WINDOW_STATE=%d,%d,%d", int(window.isVisible()), int(window.windowHandle()->isExposed()), int(window.isActiveWindow()));
        QVERIFY(window.windowHandle()->isExposed()); QVERIFY(window.isActiveWindow());
#else
        QSKIP("AppKit requires macOS");
#endif
    }
    void rhiReportsNativeDriver() {
        REQUIRE_RHI();
        TestDesktop view(false); view.resize(320, 200); view.setFrame(solidFrame(Qt::green));
        QSignalSpy submitted(&view, &DesktopView::framePresented); view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view)); QTRY_VERIFY(!submitted.isEmpty());
        QVERIFY(view.rhi()); QVERIFY(view.rhi()->backend() != QRhi::Null);
        const auto driver = view.rhi()->driverInfo();
        qInfo("RM_RHI_DRIVER=%d,%d,%s", int(view.rhi()->backend()), int(driver.deviceType), driver.deviceName.left(256).constData());
        QCOMPARE(capturedCenter(view), QColor(Qt::green));
    }
    void performanceSubmissionRetainsPaintedStamp() {
        REQUIRE_RHI();
        auto trace = std::make_shared<PerformanceTrace>(true, 9);
        TestDesktop view(false); view.resize(320, 200); view.setPerformanceTrace(trace);
        QSignalSpy presented(&view, &DesktopView::framePresented);
        view.setFrame(solidFrame(Qt::green)); view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view)); QTRY_VERIFY(!presented.isEmpty());
        const auto first = PerformanceTrace::now();
        view.afterRender = [&] {
            const auto next = PerformanceTrace::now();
            view.setFrame(solidFrame(Qt::blue), {1, 1002, next, next, 0});
            // Keep the next revision pending: this assertion concerns the painted texture,
            // not an initial-show repaint that Qt may replace before composing it.
            view.setUpdatesEnabled(false);
        };
        view.setFrame(solidFrame(Qt::red), {1, 1001, first, first, 0});
        QTRY_VERIFY([&] {
            const auto samples = trace->samples();
            return std::any_of(samples.begin(), samples.end(), [](const auto& s) {
                return s.stage == PerformanceStage::WindowSubmission && s.frame.revision == 1001;
            });
        }());
        for (const auto& sample : trace->samples()) if (sample.stage == PerformanceStage::WindowSubmission && sample.frame.revision == 1001) {
            QCOMPARE(sample.frame.latestNs, first); QVERIFY(sample.beginNs >= first);
            QVERIFY(sample.resourceEpoch > 0); QVERIFY(sample.endNs >= sample.beginNs);
        }
    }
    void performanceGrabCannotRecordWindowSubmission() {
        REQUIRE_RHI();
        auto trace = std::make_shared<PerformanceTrace>(true);
        DesktopView view(false); view.resize(320, 200); view.setPerformanceTrace(trace);
        view.setFrame(solidFrame(Qt::green));
        QVERIFY(!view.grabFramebuffer().isNull());
        QVERIFY(QMetaObject::invokeMethod(&view, "frameSubmitted", Qt::DirectConnection));
        for (const auto& sample : trace->samples()) QVERIFY(sample.stage != PerformanceStage::WindowSubmission);
    }
#ifdef RM_WITH_RDP
    void rdpDesktopHasNoInputMethodButVncDoes() {
        // macOS shows the press-and-hold accent picker only for input-method widgets.
        DesktopView rdp(false); QVERIFY(!rdp.testAttribute(Qt::WA_InputMethodEnabled));
        VncDesktopView vnc(false); QVERIFY(vnc.testAttribute(Qt::WA_InputMethodEnabled));
        QSignalSpy keys(&rdp, &DesktopView::keyInput); QSignalSpy text(&rdp, &DesktopView::textInput);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a"), repeat(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a", true);
        QApplication::sendEvent(&rdp, &press); QApplication::sendEvent(&rdp, &repeat);
        QCOMPARE(keys.count(), 2); QVERIFY(keys.at(1).at(2).toBool()); QCOMPARE(text.count(), 0); // held keys repeat on the remote
    }
    void pointerCaptureNeedsPermissionAndFrame() {
        DesktopView view(false); QSignalSpy changed(&view, &DesktopView::pointerCaptureChanged);
        view.setPointerCaptured(true); QVERIFY(!view.pointerCaptured());
        view.setPointerCaptureAllowed(true); view.setPointerCaptured(true); QVERIFY(!view.pointerCaptured()); // no desktop frame yet
        QCOMPARE(changed.count(), 0);
        // The chord without capture permission is forwarded as ordinary modifiers.
        view.setPointerCaptureAllowed(false); QSignalSpy keys(&view, &DesktopView::keyInput);
        QKeyEvent ctrl(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier), alt(QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
        QApplication::sendEvent(&view, &ctrl); QApplication::sendEvent(&view, &alt); QCOMPARE(keys.count(), 2); QVERIFY(!view.pointerCaptured());
        QVERIFY(!DesktopView::captureChordText().isEmpty());
    }
    void domainReachesRdpWorker() {
        auto* settings = freerdp_settings_new(0); QVERIFY(settings);
        SessionTarget target{"localhost", 3389, "alice", "EXAMPLE"};
        QVERIFY(RdpWorker::applyProfileSettings(settings, target, {720, 480}));
        QCOMPARE(QString::fromUtf8(freerdp_settings_get_string(settings, FreeRDP_Username)), QString("alice"));
        QCOMPARE(QString::fromUtf8(freerdp_settings_get_string(settings, FreeRDP_Domain)), QString("EXAMPLE"));
        target.username = "EXAMPLE\\alice"; QVERIFY(!RdpWorker::applyProfileSettings(settings, target, {720, 480}));
        target.domain.clear(); QVERIFY(RdpWorker::applyProfileSettings(settings, target, {720, 480}));
        QCOMPARE(QString::fromUtf8(freerdp_settings_get_string(settings, FreeRDP_Domain)), QString());
        freerdp_settings_free(settings);
    }
    void savedOptionsReachFreeRdpSettings() {
        auto* settings = freerdp_settings_new(0); QVERIFY(settings);
        SessionTarget target{"localhost", 3389, "alice"};
        QCOMPARE(bool(freerdp_settings_get_bool(settings, FreeRDP_BitmapCacheEnabled)), target.options.cacheBitmaps);
        QCOMPARE(bool(freerdp_settings_get_bool(settings, FreeRDP_ConsoleSession)), target.options.consoleSession);
        for (const auto& resolution : {QString("fit"), QString("800x600"), QString("1024x768"), QString("1280x720"), QString("1920x1080")}) {
            target.options.resolution = resolution;
            for (const bool on : {false, true}) {
                target.options.automaticResize = on; target.options.consoleSession = on; target.options.cacheBitmaps = on;
                target.options.wallpaper = on; target.options.themes = on; target.options.fontSmoothing = on;
                for (const quint8 depth : {16, 24, 32}) {
                    target.options.colorDepth = depth; QVERIFY(RdpWorker::applyProfileSettings(settings, target, {720, 480}));
                    const auto size = target.options.initialSize({720, 480});
                    QCOMPARE(freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth), quint32(size.width()));
                    QCOMPARE(freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight), quint32(size.height()));
                    QCOMPARE(freerdp_settings_get_uint32(settings, FreeRDP_ColorDepth), quint32(depth));
                    for (const auto key : {FreeRDP_ConsoleSession, FreeRDP_BitmapCacheEnabled, FreeRDP_AllowFontSmoothing, FreeRDP_SupportDisplayControl, FreeRDP_DynamicResolutionUpdate})
                        QCOMPARE(bool(freerdp_settings_get_bool(settings, key)), on);
                    QCOMPARE(bool(freerdp_settings_get_bool(settings, FreeRDP_DisableWallpaper)), !on);
                    QCOMPARE(bool(freerdp_settings_get_bool(settings, FreeRDP_DisableThemes)), !on);
                    QVERIFY(freerdp_settings_get_bool(settings, FreeRDP_NlaSecurity));
                }
            }
        }
        target.options.resolution = "invalid"; QVERIFY(!RdpWorker::applyProfileSettings(settings, target, {720, 480}));
        freerdp_settings_free(settings);
    }
#endif
    void sessionFullscreenOpensOwnWindowAndReturns() {
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.resize(900, 650); w.show(); WAIT_STORE(w);
        w.quickConnect("fullscreen.invalid", true); QPointer<SessionPane> pane = w.findChild<SessionPane*>(); QVERIFY(pane);
        w.quickConnect("other.invalid", true);
        auto* tabs = w.findChild<QTabWidget*>("sessionTabs"); tabs->setCurrentIndex(0);
        auto* connections = w.findChild<QDockWidget*>("connectionsDock"); const auto geometry = w.geometry(); const auto target = pane->target();
        auto* desktop = pane->findChild<DesktopView*>(); QVERIFY(desktop);
        const auto desktopParent = desktop->parentWidget(); const auto paneParent = pane->parentWidget();
        const auto margins = pane->layout()->contentsMargins(); const auto spacing = pane->layout()->spacing();
        auto* status = pane->findChild<QLabel*>("sessionStatus"); const bool statusHidden = status->isHidden();
        auto* action = w.findChild<QAction*>("actionTabFullscreen"); QVERIFY(action->isEnabled()); action->trigger();
        QPointer<QMainWindow> full; QTRY_VERIFY((full = fullscreenWindow(w)));
        // The session gets its own window; the main window and its docks stay as they were.
        QVERIFY(full != &w); QVERIFY(!w.isFullScreen()); QVERIFY(connections->isVisible()); QCOMPARE(w.geometry(), geometry);
        QVERIFY(full->isAncestorOf(pane)); QCOMPARE(tabs->count(), 1); QVERIFY(!w.findChild<QWidget*>("fullscreenBar"));
        auto* bar = full->findChild<QWidget*>("fullscreenBar"); QVERIFY(bar && bar->isVisible()); QVERIFY(bar->width() < 650);
        auto* row = full->findChild<QWidget*>("fullscreenControlsRow"); QVERIFY(row);
        QTRY_COMPARE(row->height(), bar->height()); QVERIFY(row->height() <= 48); QCOMPARE(row->y(), 0);
        QCOMPARE(row->parentWidget(), full->centralWidget()); QVERIFY(!pane->findChild<QWidget*>("fullscreenBar"));
        QCOMPARE(desktop->parentWidget(), desktopParent); QCOMPARE(pane->findChild<DesktopView*>(), desktop);
        QCOMPARE(pane->layout()->contentsMargins(), margins); QCOMPARE(pane->layout()->spacing(), spacing); QCOMPARE(status->isHidden(), statusHidden);
        // New connections still open in the main window.
        w.quickConnect("third.invalid", true); QCOMPARE(tabs->count(), 2); QVERIFY(full->isAncestorOf(pane)); QCOMPARE(fullscreenWindow(w), full.data());
        full->findChild<QToolButton*>("exitSessionFullscreen")->click(); QTRY_VERIFY(!w.sessionFullscreenWindow()); QTRY_VERIFY(!full || !full->isVisible());
        QCOMPARE(tabs->indexOf(pane), 0); QCOMPARE(tabs->count(), 3); QCOMPARE(tabs->currentWidget(), pane.data()); QVERIFY(tabs->tabBar()->isVisible());
        QCOMPARE(pane->parentWidget(), paneParent); QCOMPARE(desktop->parentWidget(), desktopParent); QCOMPARE(pane->findChild<DesktopView*>(), desktop);
        QCOMPARE(pane->target(), target); QCOMPARE(w.sessionCount(), size_t(3)); QVERIFY(panelWindows().isEmpty());
        // Application fullscreen is independent of session fullscreen.
        w.findChild<QAction*>("actionFullscreen")->setChecked(true); QTRY_VERIFY(w.isFullScreen());
        action->trigger(); QTRY_VERIFY((full = fullscreenWindow(w))); QVERIFY(connections->isVisible());
        full->findChild<QToolButton*>("exitSessionFullscreen")->click(); QTRY_VERIFY(!w.sessionFullscreenWindow()); QVERIFY(w.isFullScreen());
        w.findChild<QAction*>("actionFullscreen")->setChecked(false); QTRY_VERIFY(!w.isFullScreen()); QTRY_COMPARE(w.geometry(), geometry);
    }
    void nativeToggleExitsSessionFullscreen() {
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() != "cocoa") QSKIP("Native View requires Cocoa");
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.resize(900, 650); w.show(); WAIT_STORE(w);
        w.quickConnect("native-fullscreen.invalid", true); QPointer<SessionPane> pane = w.findChild<SessionPane*>(); QVERIFY(pane);
        auto* desktop = pane->findChild<DesktopView*>(); QVERIFY(desktop);
        w.findChild<QAction*>("actionTabFullscreen")->trigger(); QPointer<QMainWindow> full; QTRY_VERIFY((full = fullscreenWindow(w)));
        activateNativeTestApplication(full); full->raise(); full->activateWindow();
        // Toggling is ignored until the native enter transition finishes.
        QTRY_VERIFY_WITH_TIMEOUT(isNativeFullScreen(full), 8000); QTest::qWait(1200);
        toggleNativeFullScreen(full); QTRY_VERIFY_WITH_TIMEOUT(!w.sessionFullscreenWindow(), 8000);
        QCOMPARE(w.findChild<QTabWidget*>("sessionTabs")->indexOf(pane), 0); QCOMPARE(pane->findChild<DesktopView*>(), desktop);
        QVERIFY(!w.findChild<QWidget*>("fullscreenControlsRow")); QVERIFY(w.findChild<QDockWidget*>("connectionsDock")->isVisible());
        QVERIFY(!w.isFullScreen()); QVERIFY(!w.findChild<QAction*>("actionFullscreen")->isChecked());
#else
        QSKIP("Native View requires macOS");
#endif
    }
    void nativeSessionFullscreenMinimizesAndLeavesNoWindow() {
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() != "cocoa") QSKIP("Native fullscreen requires Cocoa");
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.resize(900, 650); w.show(); WAIT_STORE(w);
        w.quickConnect("native-minimize.invalid", true); QPointer<SessionPane> pane = w.findChild<SessionPane*>(); QVERIFY(pane);
        w.findChild<QAction*>("actionTabFullscreen")->trigger(); QPointer<QMainWindow> full; QTRY_VERIFY((full = fullscreenWindow(w)));
        QTRY_VERIFY_WITH_TIMEOUT(isNativeFullScreen(full), 8000); QTest::qWait(1200);
        // Minimize sends only the session's window to the Dock.
        full->findChild<QToolButton*>("minimizeFullscreenConnection")->click();
        QTRY_VERIFY_WITH_TIMEOUT(isNativeMiniaturized(full), 8000); QTest::qWait(1500); QVERIFY(isNativeMiniaturized(full)); QVERIFY(!isNativeMiniaturized(&w)); QVERIFY(isNativeVisible(&w));
        QCOMPARE(w.sessionFullscreenWindow(), full.data()); QVERIFY(full->isAncestorOf(pane));
        // Restoring it from the Dock returns to fullscreen.
        QTest::qWait(800); deminiaturizeNative(full);
        QTRY_VERIFY_WITH_TIMEOUT(isNativeFullScreen(full), 8000); QCOMPARE(fullscreenWindow(w), full.data()); QTest::qWait(1200);
        // Exit fullscreen returns the session and leaves no window behind.
        full->findChild<QToolButton*>("exitSessionFullscreen")->click(); QTRY_VERIFY(!w.sessionFullscreenWindow());
        QCOMPARE(w.findChild<QTabWidget*>("sessionTabs")->indexOf(pane), 0);
        QTest::qWait(2500); QVERIFY(!full || !isNativeVisible(full)); QVERIFY(!w.isFullScreen());
#else
        QSKIP("Native fullscreen requires macOS");
#endif
    }
    void fullscreenBarTargetsCapturedSession() {
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.show(); WAIT_STORE(w);
        w.quickConnect("first.invalid", true); w.quickConnect("second.invalid", true);
        auto* tabs = w.findChild<QTabWidget*>("sessionTabs"); QPointer<SessionPane> first = qobject_cast<SessionPane*>(tabs->widget(0));
        QPointer<SessionPane> second = qobject_cast<SessionPane*>(tabs->widget(1)); tabs->setCurrentIndex(1);
        tabMenuAction(w, tabs, 0, "actionTabFullscreen"); QPointer<QMainWindow> full; QTRY_VERIFY((full = fullscreenWindow(w)));
        QVERIFY(full->isAncestorOf(first)); QCOMPARE(tabs->currentWidget(), second.data());
        full->findChild<QToolButton*>("closeFullscreenConnection")->click(); QTRY_VERIFY(first.isNull());
        QVERIFY(second && second->active()); QCOMPARE(w.sessionCount(), size_t(1)); QTRY_VERIFY(!w.sessionFullscreenWindow());
        QTRY_VERIFY(panelWindows().isEmpty()); QCOMPARE(tabs->count(), 1); QVERIFY(!w.isFullScreen());
    }
    void fullscreenMinimizeAndReplacementRestoreSafely() {
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.show(); WAIT_STORE(w);
        w.quickConnect("first.invalid", true); QPointer<SessionPane> pane = w.findChild<SessionPane*>();
        auto* tabs = w.findChild<QTabWidget*>("sessionTabs");
        auto* action = w.findChild<QAction*>("actionTabFullscreen"); QVERIFY(action->isEnabled()); action->trigger();
        QPointer<QMainWindow> full; QTRY_VERIFY((full = fullscreenWindow(w)));
        // Minimize hides only the fullscreen session's window.
        full->findChild<QToolButton*>("minimizeFullscreenConnection")->click(); QTRY_VERIFY(full->isMinimized());
#ifdef Q_OS_MACOS
        // AppKit leaves fullscreen before the window reaches the Dock.
        QTRY_VERIFY_WITH_TIMEOUT(isNativeMiniaturized(full), 8000);
#endif
        QVERIFY(!w.isMinimized()); QVERIFY(w.isVisible()); QVERIFY(pane->active()); QVERIFY(full->isAncestorOf(pane)); QCOMPARE(w.sessionCount(), size_t(1));
#ifdef Q_OS_MACOS
        // Restore as a Dock click does, after the minimize animation.
        if (QGuiApplication::platformName() == "cocoa") { QTest::qWait(800); deminiaturizeNative(full); } else
#endif
        full->showNormal();
        QTRY_VERIFY_WITH_TIMEOUT(full->isFullScreen() && !full->isMinimized(), 8000); QCOMPARE(fullscreenWindow(w), full.data());
        QVERIFY(full->findChild<QWidget*>("fullscreenBar")->isVisible()); QVERIFY(full->isAncestorOf(pane));
        tabMenuAction(w, full->findChild<QTabWidget*>("panelSessionTabs"), 0, "actionTabReconnect");
        QTRY_VERIFY(pane.isNull()); QTRY_VERIFY(!w.sessionFullscreenWindow()); QCOMPARE(w.sessionCount(), size_t(1)); QCOMPARE(tabs->count(), 1);
        QVERIFY(w.findChild<QDockWidget*>("connectionsDock")->isVisible());
        action->trigger(); QTRY_VERIFY((full = fullscreenWindow(w)));
        tabMenuAction(w, full->findChild<QTabWidget*>("panelSessionTabs"), 0, "actionTabDisconnect"); QTRY_VERIFY(!w.sessionFullscreenWindow());
        QCOMPARE(tabs->count(), 1); QVERIFY(!w.findChild<SessionPane*>()->active()); QCOMPARE(w.sessionCount(), size_t(1));
        // Closing the fullscreen window ends its session.
        action->trigger(); QTRY_VERIFY((full = fullscreenWindow(w))); full->close();
        QTRY_COMPARE(w.sessionCount(), size_t(0)); QTRY_VERIFY(!w.sessionFullscreenWindow()); QTRY_VERIFY(panelWindows().isEmpty()); QCOMPARE(tabs->count(), 0);
    }
    void fullscreenLifecycleAndRequestedVisibility() {
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true, nullptr, {}, {}, {}, dir.filePath("preferences.json")); w.show(); WAIT_STORE(w);
        w.quickConnect("first.invalid", true); w.quickConnect("second.invalid", true);
        auto* tabs = w.findChild<QTabWidget*>("sessionTabs"); auto* first = qobject_cast<SessionPane*>(tabs->widget(0));
        auto* action = w.findChild<QAction*>("actionTabFullscreen"); tabs->setCurrentIndex(0); action->trigger();
        QPointer<QMainWindow> full; QTRY_VERIFY((full = fullscreenWindow(w)));
        QPointer<QToolButton> stale = full->findChild<QToolButton*>("closeFullscreenConnection"); QVERIFY(stale);
        QSignalSpy input(first, &SessionPane::input); full->findChild<QToolButton*>("exitSessionFullscreen")->click(); QTRY_VERIFY(!w.sessionFullscreenWindow());
        QCOMPARE(input.size(), 0); QCOMPARE(tabs->currentWidget(), first); action->trigger(); QTRY_VERIFY((full = fullscreenWindow(w)));
        if (stale) stale->click(); QCOMPARE(w.sessionCount(), size_t(2)); QVERIFY(fullscreenWindow(w));
        // The main window's docks and preferences work while a session is fullscreen.
        auto* connections = w.findChild<QDockWidget*>("connectionsDock"); QVERIFY(connections->isVisible());
        w.findChild<QAction*>("actionViewConnections")->setChecked(false); QVERIFY(connections->isHidden());
        w.findChild<QAction*>("actionViewNotifications")->setChecked(true); QVERIFY(w.findChild<QDockWidget*>("notificationsDock")->isVisible());
        auto* options = w.findChild<QAction*>("actionOptions"); QTRY_VERIFY(options->isEnabled()); options->trigger();
        auto* dialog = w.findChild<QDialog*>("optionsDialog"); QVERIFY(dialog); dialog->findChild<QCheckBox*>("quickConnect")->setChecked(false); dialog->accept();
        QTRY_VERIFY(QFile::exists(dir.filePath("preferences.json"))); QTRY_VERIFY(!w.findChild<QAction*>("actionViewQuickConnect")->isChecked());
        QVERIFY(w.findChild<QToolBar*>("quickConnectToolbar")->isHidden()); QVERIFY(fullscreenWindow(w));
        w.addPanel(); tabs->setCurrentIndex(1); QVERIFY(fullscreenWindow(w));
        full->showNormal(); QTRY_VERIFY(!w.sessionFullscreenWindow()); QCOMPARE(tabs->count(), 2); QTRY_VERIFY(tabs->tabBar()->isVisible());
        QVERIFY(!w.findChild<QWidget*>("fullscreenBar")); QVERIFY(connections->isHidden());
        tabs->setCurrentIndex(1); action->trigger(); QTRY_VERIFY((full = fullscreenWindow(w)));
        auto* second = qobject_cast<SessionPane*>(full->findChild<QTabWidget*>("panelSessionTabs")->widget(0)); second->event("state", "failed");
        QTRY_VERIFY(!w.sessionFullscreenWindow()); QCOMPARE(tabs->count(), 2); QCOMPARE(tabs->indexOf(second), 1);
        action->trigger(); QTRY_VERIFY(fullscreenWindow(w));
        w.close(); QCOMPARE(w.sessionCount(), size_t(0));
    }
    void fullscreenBarOverlaysAndRetractsUnlessPinned() {
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.show(); WAIT_STORE(w);
        w.quickConnect("overlay.invalid", true); auto* pane = w.findChild<SessionPane*>(); QVERIFY(pane);
        QCursor::setPos(w.mapToGlobal(QPoint(w.width() / 2, w.height() / 2)));
        w.findChild<QAction*>("actionTabFullscreen")->trigger(); QPointer<QMainWindow> full; QTRY_VERIFY((full = fullscreenWindow(w)));
        auto* controller = full->findChild<SessionFullscreenController*>("sessionFullscreenController"); QVERIFY(controller); controller->setPinned(false);
        auto* row = full->findChild<QWidget*>("fullscreenControlsRow"); QVERIFY(row);
        // The bar floats over the session instead of taking a layout row.
        QCOMPARE(full->centralWidget()->layout()->indexOf(row), -1);
        QTRY_COMPARE(QRect(pane->mapTo(full->centralWidget(), QPoint(0, 0)), pane->size()), full->centralWidget()->rect());
        auto* pin = full->findChild<QToolButton*>("pinFullscreenBar"); QVERIFY(pin && pin->isCheckable() && !pin->isChecked());
        if (QGuiApplication::platformName() == "offscreen") QSKIP("Retraction follows the real pointer");
        QTRY_VERIFY_WITH_TIMEOUT(controller->barRetracted(), 4000); QTRY_VERIFY(row->y() + row->height() <= 0);
        const QPoint edge = full->centralWidget()->mapToGlobal(QPoint(full->centralWidget()->width() / 2, 0)); QCursor::setPos(edge);
        // Warping the pointer needs Accessibility access; without it, pinning still has to reveal the bar.
        if (QTest::qWaitFor([&] { return QCursor::pos() == edge; }, 500)) { QTRY_VERIFY(!controller->barRetracted()); QTRY_COMPARE(row->y(), 0); }
        pin->click(); QVERIFY(controller->pinned()); QTRY_VERIFY(!controller->barRetracted()); QTRY_COMPARE(row->y(), 0);
        QCursor::setPos(full->mapToGlobal(QPoint(full->width() / 2, full->height() / 2))); QTest::qWait(2000);
        QVERIFY(!controller->barRetracted()); QCOMPARE(row->y(), 0);
        controller->setPinned(false);
        full->findChild<QToolButton*>("exitSessionFullscreen")->click(); QTRY_VERIFY(!w.sessionFullscreenWindow());
    }
    void fullscreenTargetDestructionRemovesShellControls() {
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.show(); WAIT_STORE(w);
        w.quickConnect("destroyed.invalid", true); auto* pane = w.findChild<SessionPane*>(); QVERIFY(pane);
        w.findChild<QAction*>("actionTabFullscreen")->trigger(); QPointer<QMainWindow> full; QTRY_VERIFY((full = fullscreenWindow(w)));
        QPointer<QWidget> row = full->findChild<QWidget*>("fullscreenControlsRow"); QVERIFY(row);
        delete pane;
        QTRY_VERIFY(!w.sessionFullscreenWindow()); QTRY_VERIFY(row.isNull()); QTRY_VERIFY(panelWindows().isEmpty());
        QVERIFY(!w.isFullScreen()); QVERIFY(w.findChild<QDockWidget*>("connectionsDock")->isVisible());
    }
    void desktopShortcutsRemainRemote() {
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.show(); WAIT_STORE(w);
        w.quickConnect("keys.invalid", true); auto* view = w.findChild<DesktopView*>(); QVERIFY(view);
        w.findChild<QAction*>("actionTabFullscreen")->trigger(); QPointer<QMainWindow> full; QTRY_VERIFY((full = fullscreenWindow(w)));
        full->raise(); full->activateWindow(); view->setFocus(); QTRY_VERIFY(view->hasFocus());
        QSignalSpy remote(view, &DesktopView::mockInput);
        for (const auto chord : {QKeySequence(Qt::CTRL | Qt::Key_Comma), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Return), QKeySequence(Qt::Key_Escape), QKeySequence(Qt::Key_F11)})
            QTest::keySequence(view, chord);
        QVERIFY(full->isFullScreen()); QVERIFY(!w.findChild<QDialog*>("optionsDialog"));
        for (const auto key : {Qt::Key_Comma, Qt::Key_Return, Qt::Key_Escape, Qt::Key_F11}) {
            bool down = false, up = false;
            for (const auto& event : remote) {
                down |= event[0].toString() == QString("Key %1 down").arg(key);
                up |= event[0].toString() == QString("Key %1 up").arg(key);
            }
            QVERIFY(down && up);
        }
        full->findChild<QToolButton*>("exitSessionFullscreen")->click(); QTRY_VERIFY(!w.sessionFullscreenWindow());
    }
    void dragSessionTabOutCreatesWindowAndDragBackMerges() {
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.resize(1000, 700); w.show(); WAIT_STORE(w);
        w.quickConnect("first.invalid", true); w.quickConnect("second.invalid", true);
        auto* tabs = w.findChild<QTabWidget*>("sessionTabs"); QPointer<SessionPane> second = qobject_cast<SessionPane*>(tabs->widget(1)); const auto session = second->id();
        auto* desktop = second->findChild<DesktopView*>();
        // Small vertical moves stay in the strip (reordering); leaving it tears the tab into a new window.
        dragTab(tabs->tabBar(), 1, {tabs->tabBar()->mapToGlobal(tabs->tabBar()->tabRect(1).center() + QPoint(0, 10)), w.mapToGlobal(QPoint(500, 500)), w.mapToGlobal(QPoint(520, 520))});
        QTRY_COMPARE(panelWindows().size(), 1); auto* window = panelWindows().first(); auto* groups = window->findChild<QTabWidget*>("windowGroupTabs");
        QCOMPARE(groups->count(), 1); auto* moved = groups->widget(0)->findChild<QTabWidget*>(); QCOMPARE(moved->widget(0), second.data());
        QCOMPARE(tabs->count(), 1); QCOMPARE(second->id(), session); QCOMPARE(second->findChild<DesktopView*>(), desktop); QCOMPARE(w.sessionCount(), size_t(2));
        // Dragging the only tab moves the whole window; dropping it on the main strip merges it back.
        // Past the last tab, still within the strip's row, appends.
        const auto strip = tabs->mapToGlobal(QPoint(tabs->width() - 20, tabs->tabBar()->y() + tabs->tabBar()->height() / 2));
        // Over a strip, the area the tab will join is highlighted, as when docking.
        dragTab(moved->tabBar(), 0, {moved->tabBar()->mapToGlobal(QPoint(10, -60)), strip}, [&] {
            auto* band = w.findChild<QWidget*>("tabDropHighlight"); QVERIFY(band && band->isVisible()); QCOMPARE(band->parentWidget(), &w);
            QCOMPARE(band->geometry(), QRect(tabs->mapTo(&w, QPoint(0, 0)), tabs->size()));
        });
        QVERIFY(!w.findChild<QWidget*>("tabDropHighlight")->isVisible());
        QTRY_VERIFY(panelWindows().isEmpty()); QCOMPARE(tabs->count(), 2); QCOMPARE(tabs->widget(1), second.data()); QCOMPARE(second->id(), session);
        QCOMPARE(w.sessionCount(), size_t(2));
    }
    void dragSessionTabOntoAnotherPanel() {
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.resize(1000, 700); w.show(); WAIT_STORE(w);
        auto* groups = w.findChild<QTabWidget*>("groupTabs"); w.addPanel(); QPointer<QWidget> panel = groups->currentWidget(); auto* panelTabs = panel->findChild<QTabWidget*>();
        groups->setCurrentIndex(0); w.quickConnect("first.invalid", true); w.quickConnect("second.invalid", true);
        auto* tabs = w.findChild<QTabWidget*>("sessionTabs"); QPointer<SessionPane> first = qobject_cast<SessionPane*>(tabs->widget(0));
        const auto panelTab = groups->tabBar()->mapToGlobal(groups->tabBar()->tabRect(1).center());
        dragTab(tabs->tabBar(), 0, {panelTab}, [&] {
            // Only the panel tab the session joins is highlighted.
            auto* band = w.findChild<QWidget*>("tabDropHighlight"); QVERIFY(band && band->isVisible());
            const auto tab = groups->tabBar()->tabRect(1); QCOMPARE(band->geometry(), QRect(groups->tabBar()->mapTo(&w, tab.topLeft()), tab.size()));
        });
        QTRY_COMPARE(panelTabs->indexOf(first), 0); QCOMPARE(tabs->count(), 1); QVERIFY(panelWindows().isEmpty());
        // Emptying a panel by dragging its last tab away removes the panel; General stays.
        groups->setCurrentWidget(panel); const auto generalTab = groups->tabBar()->mapToGlobal(groups->tabBar()->tabRect(0).center());
        dragTab(panelTabs->tabBar(), 0, {generalTab});
        QTRY_VERIFY(panel.isNull()); QCOMPARE(tabs->count(), 2); QCOMPARE(groups->count(), 1); QCOMPARE(w.sessionCount(), size_t(2));
    }
    void dragPanelOutAndBack() {
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.resize(1000, 700); w.show(); WAIT_STORE(w);
        auto* groups = w.findChild<QTabWidget*>("groupTabs"); w.addPanel(); QPointer<QWidget> panel = groups->currentWidget();
        w.quickConnect("panel.invalid", true); QPointer<SessionPane> pane = panel->findChild<SessionPane*>(); QVERIFY(pane);
        QVERIFY(!dragTab(groups->tabBar(), 0, {w.mapToGlobal(QPoint(500, 500))})); QVERIFY(panelWindows().isEmpty());
        dragTab(groups->tabBar(), 1, {w.mapToGlobal(QPoint(500, 500)), w.mapToGlobal(QPoint(520, 520))});
        QTRY_COMPARE(panelWindows().size(), 1); auto* windowGroups = panelWindows().first()->findChild<QTabWidget*>("windowGroupTabs");
        QCOMPARE(windowGroups->indexOf(panel), 0); QCOMPARE(groups->count(), 1); QCOMPARE(panel->findChild<SessionPane*>(), pane.data());
        const auto mainPanels = groups->mapToGlobal(QPoint(groups->width() - 20, groups->tabBar()->y() + groups->tabBar()->height() / 2));
        dragTab(windowGroups->tabBar(), 0, {windowGroups->tabBar()->mapToGlobal(QPoint(10, -60)), mainPanels}, [&] {
            auto* band = w.findChild<QWidget*>("tabDropHighlight"); QVERIFY(band && band->isVisible());
            QCOMPARE(band->geometry().y(), groups->tabBar()->mapTo(&w, QPoint(0, 0)).y()); QCOMPARE(band->width(), groups->width());
        });
        QTRY_VERIFY(panelWindows().isEmpty()); QCOMPARE(groups->indexOf(panel), 1); QCOMPARE(w.sessionCount(), size_t(1));
    }
    void newSessionsOpenInLastUsedWindowAndWindowCloseEndsItsSessions() {
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.resize(1000, 700); w.show(); WAIT_STORE(w);
        w.quickConnect("main.invalid", true); w.quickConnect("moved.invalid", true);
        auto* tabs = w.findChild<QTabWidget*>("sessionTabs"); QPointer<SessionPane> kept = qobject_cast<SessionPane*>(tabs->widget(0));
        dragTab(tabs->tabBar(), 1, {w.mapToGlobal(QPoint(500, 500)), w.mapToGlobal(QPoint(520, 520))}); QTRY_COMPARE(panelWindows().size(), 1);
        auto* window = panelWindows().first(); auto* moved = window->findChild<QTabWidget*>("panelSessionTabs");
        // Pressing a window's tab makes it the target for new connections.
        QTest::mousePress(moved->tabBar(), Qt::LeftButton, Qt::NoModifier, moved->tabBar()->tabRect(0).center());
        QTest::mouseRelease(moved->tabBar(), Qt::LeftButton, Qt::NoModifier, moved->tabBar()->tabRect(0).center());
        w.quickConnect("third.invalid", true); QCOMPARE(moved->count(), 2); QCOMPARE(tabs->count(), 1);
        QTest::mousePress(tabs->tabBar(), Qt::LeftButton, Qt::NoModifier, tabs->tabBar()->tabRect(0).center());
        QTest::mouseRelease(tabs->tabBar(), Qt::LeftButton, Qt::NoModifier, tabs->tabBar()->tabRect(0).center());
        w.quickConnect("fourth.invalid", true); QCOMPARE(tabs->count(), 2); QCOMPARE(w.sessionCount(), size_t(4));
        // A tab in another window goes fullscreen in its own window and returns to that window.
        moved->setCurrentIndex(0); QPointer<SessionPane> inWindow = qobject_cast<SessionPane*>(moved->widget(0));
        tabMenuAction(w, moved, 0, "actionTabFullscreen"); QPointer<QMainWindow> full; QTRY_VERIFY((full = fullscreenWindow(w)));
        QVERIFY(full != window); QVERIFY(!window->isFullScreen()); QVERIFY(!w.isFullScreen()); QVERIFY(full->isAncestorOf(inWindow)); QCOMPARE(moved->count(), 1);
        full->findChild<QToolButton*>("exitSessionFullscreen")->click(); QTRY_VERIFY(!w.sessionFullscreenWindow());
        QCOMPARE(moved->indexOf(inWindow), 0); QCOMPARE(moved->count(), 2); QTRY_COMPARE(panelWindows().size(), 1);
        // Closing the window ends only its sessions.
        window->close(); QTRY_VERIFY(panelWindows().isEmpty()); QVERIFY(inWindow.isNull()); QVERIFY(kept && kept->active()); QCOMPARE(w.sessionCount(), size_t(2));
    }
    void optionsApplyCancelAndRestart() {
        QTemporaryDir dir; const auto preferences = dir.filePath("preferences.json"), connections = dir.filePath("profiles.json");
        {
            MainWindow w(connections, true, nullptr, {}, {}, {}, preferences); w.show(); WAIT_STORE(w);
            auto* action = w.findChild<QAction*>("actionOptions"); QTRY_VERIFY(action->isEnabled()); action->trigger();
            auto* dialog = w.findChild<QDialog*>("optionsDialog"); QVERIFY(dialog);
            dialog->findChild<QCheckBox*>("categorized")->setChecked(false); dialog->findChild<QCheckBox*>("propertyHelp")->setChecked(false);
            dialog->findChild<QCheckBox*>("quickConnect")->setChecked(false); dialog->findChild<QCheckBox*>("lockToolbars")->setChecked(true);
            dialog->reject(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QVERIFY(w.findChild<ProfileEditor*>()->categorized()); QVERIFY(!QFile::exists(preferences));
            action->trigger(); dialog = w.findChild<QDialog*>("optionsDialog"); QVERIFY(dialog);
            dialog->findChild<QCheckBox*>("categorized")->setChecked(false); dialog->findChild<QCheckBox*>("propertyHelp")->setChecked(false);
            dialog->findChild<QCheckBox*>("quickConnect")->setChecked(false); dialog->findChild<QCheckBox*>("lockToolbars")->setChecked(true);
            dialog->accept(); QTRY_VERIFY(QFile::exists(preferences)); QTRY_VERIFY(!w.findChild<ProfileEditor*>()->categorized());
            QVERIFY(w.findChild<QToolBar*>("quickConnectToolbar")->isHidden()); QVERIFY(!w.findChild<QToolBar*>("quickConnectToolbar")->isMovable());
            QVERIFY(w.findChild<QWidget*>("configHelp")->isHidden()); QVERIFY(!QFile::exists(connections));
        }
        MainWindow restart(connections, true, nullptr, {}, {}, {}, preferences); restart.show(); WAIT_STORE(restart);
        QTRY_VERIFY(restart.findChild<QAction*>("actionOptions")->isEnabled());
        QVERIFY(!restart.findChild<ProfileEditor*>()->categorized()); QVERIFY(restart.findChild<QToolBar*>("quickConnectToolbar")->isHidden());
        QVERIFY(restart.findChild<QWidget*>("configHelp")->isHidden()); QVERIFY(!restart.findChild<QToolBar*>("quickConnectToolbar")->isMovable());
        QVERIFY(restart.findChild<QDockWidget*>("notificationsDock")->isHidden());
    }
    void darkIconsSwapGlyphsAndRestore() {
        QImage glyph(16, 16, QImage::Format_ARGB32); glyph.fill(QColor("#f6f6f6"));
        for (int y = 4; y < 8; ++y) for (int x = 4; x < 8; ++x) glyph.setPixel(x, y, QColor("#424242").rgb());
        glyph.setPixel(0, 0, QColor("#1ba1e2").rgb());
        const auto swapped = ThemeIcons::dark(glyph);
        QCOMPARE(QColor(swapped.pixel(5, 5)), QColor("#f6f6f6")); QCOMPARE(QColor(swapped.pixel(12, 12)), QColor("#252525"));
        QCOMPARE(QColor(swapped.pixel(0, 0)), QColor("#1ba1e2"));
        // Full-color pictures without the glyph gray are left alone.
        QImage picture(16, 16, QImage::Format_ARGB32); picture.fill(QColor("#c03030")); for (int x = 0; x < 16; ++x) picture.setPixel(x, 1, QColor("#424242").rgb());
        QCOMPARE(ThemeIcons::dark(picture), picture);
        const QIcon light(QPixmap::fromImage(glyph));
        applyTheme(*qApp, "dark"); const auto dark = ThemeIcons::themed(light); QVERIFY(dark.cacheKey() != light.cacheKey());
        QCOMPARE(ThemeIcons::themed(dark).cacheKey(), dark.cacheKey());
        applyTheme(*qApp, "light"); QCOMPARE(ThemeIcons::themed(dark).cacheKey(), light.cacheKey());
    }
    void darkThemePersistsAndRestyles() {
        QTemporaryDir dir; const auto preferences = dir.filePath("preferences.json");
        const auto choose = [&](MainWindow& w, const QString& theme) {
            auto* action = w.findChild<QAction*>("actionOptions"); [&] { QTRY_VERIFY(action->isEnabled()); }(); action->trigger();
            auto* dialog = w.findChild<QDialog*>("optionsDialog"); auto* combo = dialog->findChild<QComboBox*>("theme");
            combo->setCurrentIndex(combo->findData(theme)); dialog->accept(); [&] { QTRY_VERIFY(!w.findChild<QDialog*>("optionsDialog")); }();
        };
        {
            MainWindow w(dir.filePath("profiles.json"), true, nullptr, {}, {}, {}, preferences); w.show(); WAIT_STORE(w);
            QVERIFY(qApp->palette().color(QPalette::Window).lightness() > 128); const auto icon = w.findChild<QAction*>("actionOptions")->icon().cacheKey(); choose(w, "dark");
            QVERIFY(w.findChild<QAction*>("actionOptions")->icon().cacheKey() != icon);
            QVERIFY(qApp->palette().color(QPalette::Window).lightness() < 128);
            QVERIFY(w.styleSheet().contains("#2b2b2b")); QVERIFY(!w.styleSheet().contains("#f0f0f0"));
        }
        QFile stored(preferences); QVERIFY(stored.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(stored.readAll()).object().value("theme"), QJsonValue("dark"));
        MainWindow restart(dir.filePath("profiles.json"), true, nullptr, {}, {}, {}, preferences); restart.show(); WAIT_STORE(restart);
        QTRY_VERIFY(restart.styleSheet().contains("#2b2b2b"));
        const auto darkIcon = restart.findChild<QAction*>("actionOptions")->icon(); QCOMPARE(QColor(darkIcon.pixmap(16, 16).toImage().pixel(8, 8)).lightness() >= 0, true);
        choose(restart, "light"); QVERIFY(qApp->palette().color(QPalette::Window).lightness() > 128);
        QVERIFY(restart.findChild<QAction*>("actionOptions")->icon().cacheKey() != darkIcon.cacheKey()); QVERIFY(restart.styleSheet().contains("#f0f0f0"));
    }
    void openh264OptionCreditsCiscoAndPersists() {
        QTemporaryDir dir; const auto preferences = dir.filePath("preferences.json");
        const auto open = [](MainWindow& w) {
            auto* action = w.findChild<QAction*>("actionOptions"); [&] { QTRY_VERIFY(action->isEnabled()); }(); action->trigger();
            return w.findChild<QDialog*>("optionsDialog");
        };
        {
            MainWindow w(dir.filePath("profiles.json"), true, nullptr, {}, {}, {}, preferences); w.show(); WAIT_STORE(w);
            auto* dialog = open(w); QVERIFY(dialog);
            auto* box = dialog->findChild<QCheckBox*>("openh264"); QVERIFY(box); QVERIFY(box->isChecked());
            QCOMPARE(dialog->findChild<QLabel*>("openh264Credit")->text(), QString("OpenH264 Video Codec provided by Cisco Systems, Inc."));
            box->setChecked(false); dialog->accept(); QTRY_VERIFY(QFile::exists(preferences));
            QTRY_VERIFY(!w.findChild<QDialog*>("optionsDialog"));
        }
        QFile stored(preferences); QVERIFY(stored.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(stored.readAll()).object().value("openh264"), QJsonValue(false));
        MainWindow restart(dir.filePath("profiles.json"), true, nullptr, {}, {}, {}, preferences); restart.show(); WAIT_STORE(restart);
        auto* dialog = open(restart); QVERIFY(dialog); QVERIFY(!dialog->findChild<QCheckBox*>("openh264")->isChecked());
        dialog->reject();
    }
    void preferencesStayInExplicitDataDirectory() {
        QTemporaryDir dir; QDir root(dir.path()); QVERIFY(root.mkdir("app-data") && root.mkdir("connections"));
        const auto preferences = root.filePath("app-data/preferences.json"), connections = root.filePath("connections/profiles.json");
        MainWindow w(connections, true, nullptr, {}, {}, {}, preferences); w.show(); WAIT_STORE(w);
        auto* action = w.findChild<QAction*>("actionOptions"); QTRY_VERIFY(action->isEnabled()); action->trigger();
        w.findChild<QDialog*>("optionsDialog")->accept(); QTRY_VERIFY(QFile::exists(preferences));
        QVERIFY(QDir(root.filePath("connections")).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
        QFile stored(preferences); QVERIFY(stored.open(QIODevice::ReadOnly)); const auto bytes = stored.readAll();
        QVERIFY(!bytes.contains("password") && !bytes.contains("notifications") && !bytes.contains("fullscreen"));
        QTemporaryDir failed; MainWindow bad(failed.filePath("profiles.json"), true, nullptr, {}, {}, {}, failed.filePath("missing/preferences.json"));
        bad.show(); WAIT_STORE(bad); QTRY_VERIFY(bad.findChild<QAction*>("actionOptions")->isEnabled()); bad.findChild<QAction*>("actionOptions")->trigger();
        auto* dialog = bad.findChild<QDialog*>("optionsDialog"); QVERIFY(dialog); dialog->findChild<QCheckBox*>("categorized")->setChecked(false); dialog->accept();
        QTRY_VERIFY(!dialog->findChild<QLabel*>("error")->text().isEmpty()); QVERIFY(dialog->isVisible()); QVERIFY(bad.findChild<ProfileEditor*>()->categorized());
        dialog->reject();
    }
    void nativeOptionsShortcutWithDesktopFocus() {
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() != "cocoa") QSKIP("Native Preferences requires Cocoa");
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.show(); WAIT_STORE(w);
        auto* action = w.findChild<QAction*>("actionOptions"); QTRY_VERIFY(action->isEnabled());
        w.quickConnect("shortcut.invalid", true); auto* view = w.findChild<DesktopView*>(); QVERIFY(view);
        QSignalSpy remote(view, &DesktopView::mockInput);
        for (int i = 0; i < 2; ++i) {
            activateNativeTestApplication(&w); w.raise(); w.activateWindow(); view->setFocus(); QTRY_VERIFY(view->hasFocus());
            QVERIFY(triggerNativePreferencesShortcut()); QTRY_VERIFY(w.findChild<QDialog*>("optionsDialog"));
            w.findChild<QDialog*>("optionsDialog")->reject(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        for (const auto& input : remote) QVERIFY(!input[0].toString().contains(QString::number(Qt::Key_Comma)));
#else
        QSKIP("Native Preferences requires macOS");
#endif
    }
    void notificationsHiddenOnLaunchAndReset() {
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.show(); WAIT_STORE(w);
        auto* dock = w.findChild<QDockWidget*>("notificationsDock"); auto* action = w.findChild<QAction*>("actionViewNotifications");
        QVERIFY(dock->isHidden()); QVERIFY(!action->isChecked());
        w.quickConnect("first.invalid", true); QTRY_VERIFY(w.findChild<QListWidget*>("notificationsList")->count() > 0);
        QVERIFY(dock->isHidden()); action->trigger(); QTRY_VERIFY(dock->isVisible());
        w.quickConnect("second.invalid", true); QVERIFY(dock->isVisible());
        w.findChild<QAction*>("actionResetLayout")->trigger(); QTRY_VERIFY(dock->isHidden()); QVERIFY(!action->isChecked());
    }
    void firstSessionPreservesDockLayout() {
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.resize(900, 650); w.show(); WAIT_STORE(w);
        auto* connections = w.findChild<QDockWidget*>("connectionsDock"); auto* config = w.findChild<QDockWidget*>("configDock");
        QTRY_VERIFY(connections->isVisible() && config->isVisible());
        QCOMPARE(connections->x(), config->x()); QCOMPARE(connections->width(), config->width());
        QVERIFY(connections->geometry().bottom() < config->geometry().top());
        w.quickConnect("first.invalid", true); QTest::qWait(80);
        QVERIFY(connections->isVisible() && config->isVisible()); QCOMPARE(connections->x(), config->x());
        QVERIFY(connections->width() >= 200 && config->height() >= 130);
        config->hide(); w.quickConnect("second.invalid", true); QTest::qWait(80);
        QVERIFY(connections->isVisible()); QVERIFY(config->isHidden());
        w.hide(); w.show(); QTest::qWait(80); QVERIFY(connections->isVisible()); QVERIFY(config->isHidden());
    }
    void longSessionStatusDoesNotSqueezeDocks() {
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.resize(900, 650); w.show(); WAIT_STORE(w);
        w.quickConnect("first.invalid", true); QTest::qWait(80);
        auto* pane = w.findChild<SessionPane*>(); QVERIFY(pane);
        auto* connections = w.findChild<QDockWidget*>("connectionsDock"); auto* config = w.findChild<QDockWidget*>("configDock");
        const auto width = w.width(); const auto dockWidth = connections->width();
        pane->event("state", QString(2048, 'x')); QVERIFY(pane->minimumSizeHint().width() < 700); QTest::qWait(80);
        QCOMPARE(w.width(), width); QCOMPARE(connections->width(), dockWidth); QVERIFY(config->isVisible());
        QVERIFY(pane->minimumSizeHint().width() < 700);
    }
    void quickConnectProtocolMenuAndCompactControls() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); MainWindow w(path, true); w.resize(1200, 650); w.show(); WAIT_STORE(w);
        auto* button = w.findChild<QToolButton*>("quickConnectButton"); auto* combo = w.findChild<QComboBox*>("quickConnectCombo");
        auto* bar = w.findChild<QToolBar*>("quickConnectToolbar"); QVERIFY(!w.findChild<QComboBox*>("quickConnectProtocol"));
        QCOMPARE(button->popupMode(), QToolButton::MenuButtonPopup); QVERIFY(button->menu());
        QStringList names; for (auto* action : button->menu()->actions()) names << action->text();
        QCOMPARE(names, QStringList({"RDP", "VNC", "SSH"}));
        for (auto* action : button->menu()->actions()) QVERIFY(action->isEnabled());
        QVERIFY(button->menu()->actions().at(0)->isChecked()); QCOMPARE(button->text(), QString("RDP"));
        button->menu()->actions().at(2)->trigger(); QCOMPARE(button->text(), QString("SSH")); QCOMPARE(w.quickConnectProtocol(), QString("ssh"));
        QCOMPARE(w.sessionCount(), size_t(0)); QVERIFY(!QFile::exists(path));
        button->menu()->actions().at(1)->trigger(); QCOMPARE(button->text(), QString("VNC")); QCOMPARE(w.quickConnectProtocol(), QString("vnc"));
        w.setQuickConnectProtocol("telnet"); QCOMPARE(w.quickConnectProtocol(), QString("vnc"));
        button->menu()->actions().at(0)->trigger(); QCOMPARE(button->text(), QString("RDP")); QVERIFY(combo->isEditable());
        QVERIFY(bar->width() < w.width() / 2); QVERIFY(combo->width() <= 360);
        w.quickConnect("first.invalid", true); w.quickConnect("second.invalid", true); QVERIFY(combo->findText("first.invalid") >= 0); QVERIFY(combo->findText("second.invalid") >= 0);
        w.resize(640, 480); QTest::qWait(80); QVERIFY(button->isVisible()); QVERIFY(combo->width() >= 150);
    }
    void globeMenuListsFilesFoldersAndConnections() {
        QTemporaryDir dir; MainWindow w(dir.filePath("confCons.xml"), true); w.resize(1200, 650); w.show(); WAIT_STORE(w);
        auto* globe = w.findChild<QToolButton*>("savedConnectionsButton"); QVERIFY(globe->isVisible()); QCOMPARE(globe->popupMode(), QToolButton::InstantPopup);
        auto* tree = w.findChild<QTreeWidget*>("connectionTree");
        w.addFolder(); WAIT_STORE(w); const auto folder = w.selectedId(); setGridText(w, "name", "Folder"); WAIT_STORE(w);
        w.addConnection(); WAIT_STORE(w); setGridText(w, "name", "Inside"); WAIT_STORE(w); setGridText(w, "host", "inside.invalid"); WAIT_STORE(w);
        tree->setCurrentItem(tree->topLevelItem(0)); w.addFolder(); WAIT_STORE(w); setGridText(w, "name", "Empty"); WAIT_STORE(w);
        auto* menu = globe->menu(); QVERIFY(menu); emit menu->aboutToShow();
        QCOMPARE(menu->actions().size(), 1); auto* root = menu->actions().at(0)->menu(); QVERIFY(root); QVERIFY(menu->actions().at(0)->text().startsWith("Connections"));
        QStringList names; for (auto* action : root->actions()) names << action->text(); QCOMPARE(names, QStringList({"Folder", "Empty"}));
        QVERIFY(root->actions().at(0)->menu()); QVERIFY(!root->actions().at(1)->isEnabled());
        auto* inside = root->actions().at(0)->menu()->actions().at(0); QCOMPARE(inside->text(), QString("Inside"));
#ifdef RM_WITH_RDP
        QVERIFY(inside->isEnabled()); tree->setCurrentItem(tree->topLevelItem(0)); inside->trigger();
        QTRY_VERIFY(w.findChild<QDialog*>("CredentialsDialog")); QCOMPARE(w.selectedId().isEmpty(), false);
        w.findChild<QDialog*>("CredentialsDialog")->reject();
#endif
        Q_UNUSED(folder);
    }
    void viewMenuMatchesOriginalOrder() {
        QTemporaryDir dir; MainWindow w(dir.filePath("confCons.xml"), true); w.show(); WAIT_STORE(w);
        QStringList top; for (auto* action : w.menuBar()->actions()) top << action->text().remove('&');
        QCOMPARE(top, QStringList({"File", "View", "Tools", "Help"}));
        QStringList order; for (auto* action : w.findChild<QMenu*>("menuView")->actions()) order << (action->isSeparator() ? "|" : action->text());
        QCOMPARE(order, QStringList({"Add Connection Panel", "Connection Panels", "Panel Tabs", "|", "Connections", "Config", "Notifications", "Screenshots", "|", "Jump To", "Reset layout", "Lock toolbar positions", "|", "Quick Connect Toolbar", "External Tools Toolbar", "Multi SSH Toolbar", "|", "Full Screen"}));
        auto* file = w.findChild<QMenu*>("menuFile");
        for (const auto* name : {"actionSaveAsConnection", "actionSaveFile", "actionSaveFileAs", "actionCloseFile"})
            QVERIFY(file->actions().contains(w.findChild<QAction*>(name)));
        QVERIFY(w.findChild<QMenu*>("menuTools")->actions().contains(w.findChild<QAction*>("actionDifferentCredentials")));
        QCOMPARE(w.findChild<QAction*>("actionFullscreen")->shortcut(), QKeySequence(Qt::Key_F11));
    }
    void jumpToFocusesExistingTargetsWithoutConnecting() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); MainWindow w(path, true); w.show(); WAIT_STORE(w);
        w.quickConnect("localhost:3391", true); w.quickConnect("localhost:3392", true);
        auto* tabs = w.findChild<QTabWidget*>("sessionTabs"); auto* groups = w.findChild<QTabWidget*>("groupTabs");
        auto* first = qobject_cast<SessionPane*>(tabs->widget(0)); auto* second = qobject_cast<SessionPane*>(tabs->widget(1)); QVERIFY(first); QVERIFY(second);
        const auto firstId = first->id(), secondId = second->id(); const auto count = w.sessionCount(); w.addPanel(); auto* panel = groups->currentWidget(); auto* general = groups->widget(0);
        auto* jump = w.findChild<QMenu*>("menuJumpTo"); QVERIFY(jump); jump->popup(w.mapToGlobal(w.rect().center())); QTRY_VERIFY(jump->actions().size() >= 9); jump->hide();
        QAction *toFirst = nullptr, *toGeneral = nullptr, *toConnections = nullptr;
        for (auto* action : jump->actions()) {
            if (action->data().toString() == firstId) toFirst = action;
            if (action->data().value<QWidget*>() == general) toGeneral = action;
            if (action->data().toString() == "connectionsDock") toConnections = action;
        }
        QVERIFY(toFirst); QVERIFY(toGeneral); QVERIFY(toConnections);
        tabs->tabBar()->moveTab(0, 1); groups->tabBar()->moveTab(0, 1); toFirst->trigger(); QCOMPARE(tabs->currentWidget(), first); QCOMPARE(groups->currentWidget(), general);
        QCOMPARE(first->id(), firstId); QCOMPARE(second->id(), secondId); QCOMPARE(w.sessionCount(), count); QVERIFY(!QFile::exists(path)); QVERIFY(!QFile::exists(path + ".vault"));
        groups->setCurrentWidget(panel); toGeneral->trigger(); QCOMPARE(groups->currentWidget(), general);
        auto* dock = w.findChild<QDockWidget*>("connectionsDock"); dock->hide(); toConnections->trigger(); QTRY_VERIFY(dock->isVisible());
        const auto point = tabs->tabBar()->tabRect(tabs->indexOf(first)).center();
        QContextMenuEvent context(QContextMenuEvent::Mouse, point, tabs->tabBar()->mapToGlobal(point)); QApplication::sendEvent(tabs->tabBar(), &context);
        auto* popup = w.findChild<QMenu*>("sessionContextMenu"); QVERIFY(popup); auto* move = popup->findChild<QMenu*>("moveSessionMenu"); QVERIFY(move);
        for (auto* action : move->actions()) if (action->isEnabled()) { action->trigger(); break; } popup->close();
        auto* other = panel->findChild<QTabWidget*>(); QCOMPARE(other->widget(0), first); groups->setCurrentWidget(general); toFirst->trigger(); QCOMPARE(groups->currentWidget(), panel); QCOMPARE(other->currentWidget(), first);
        QVERIFY(w.focusWidget() == first || first->isAncestorOf(w.focusWidget()));
        w.closeTab(other->indexOf(first)); const auto afterClose = w.sessionCount(); toFirst->trigger(); QCOMPARE(w.sessionCount(), afterClose); QCOMPARE(tabs->count(), 1); QCOMPARE(second->id(), secondId);
    }
    void visibilityChecksTrackDocksAndToolbars() {
        QTemporaryDir dir; MainWindow w(dir.filePath("confCons.xml"), true); w.show(); WAIT_STORE(w);
        for (const auto& pair : {qMakePair("connectionsDock", "actionViewConnections"), qMakePair("configDock", "actionViewConfig"), qMakePair("notificationsDock", "actionViewNotifications"), qMakePair("quickConnectToolbar", "actionViewQuickConnect"), qMakePair("externalToolsToolbar", "actionViewExternalTools"), qMakePair("multiSshToolbar", "actionViewMultiSsh")}) {
            auto* target = w.findChild<QWidget*>(pair.first); auto* action = w.findChild<QAction*>(pair.second); QVERIFY(target); QVERIFY(action);
            target->show(); QTRY_VERIFY(action->isChecked()); target->hide(); QTRY_VERIFY(!action->isChecked()); action->trigger(); QTRY_VERIFY(target->isVisible());
        }
        w.findChild<QAction*>("actionResetLayout")->trigger(); QVERIFY(w.findChild<QAction*>("actionViewConnections")->isChecked()); QVERIFY(w.findChild<QAction*>("actionViewQuickConnect")->isChecked()); QVERIFY(!w.findChild<QAction*>("actionViewExternalTools")->isChecked());
    }
    void updateVersionsCompareNumerically() {
        QCOMPARE(Updates::compareVersions("0.9.0", "0.10.0"), std::optional<int>(-1));
        QCOMPARE(Updates::compareVersions("v1.2", "1.2.0"), std::optional<int>(0));
        QCOMPARE(Updates::compareVersions("1.2.3-beta", "1.2.2"), std::optional<int>(1));
        QVERIFY(!Updates::compareVersions("latest", "1.0.0"));
        const auto release = Updates::parseLatestRelease("{\"tag_name\":\"v0.2.0\",\"html_url\":\"https://github.com/Crayjones/CMRemote/releases/tag/v0.2.0\"}");
        QVERIFY(release); QCOMPARE(release->version, QString("0.2.0"));
        // Only this project's release pages are offered as download links.
        QVERIFY(!Updates::parseLatestRelease("{\"tag_name\":\"v9.0.0\",\"html_url\":\"https://example.com/CMRemote\"}"));
        // Assets: only this repository's downloads, with GitHub's SHA-256 digest.
        const auto digest = QString(64, 'a');
        const auto withAssets = Updates::parseLatestRelease(QString("{\"tag_name\":\"v0.3.0\",\"html_url\":\"https://github.com/Crayjones/CMRemote/releases/tag/v0.3.0\",\"assets\":["
            "{\"name\":\"CMRemote-0.3.0-macos-arm64.dmg\",\"browser_download_url\":\"https://github.com/Crayjones/CMRemote/releases/download/v0.3.0/CMRemote-0.3.0-macos-arm64.dmg\",\"digest\":\"sha256:%1\"},"
            "{\"name\":\"evil.dmg\",\"browser_download_url\":\"https://example.com/evil.dmg\"},"
            "{\"name\":\"../x\",\"browser_download_url\":\"https://github.com/Crayjones/CMRemote/releases/download/v0.3.0/x\"}]}").arg(digest).toUtf8());
        QVERIFY(withAssets); QCOMPARE(withAssets->assets.size(), 1); QCOMPARE(withAssets->assets.first().sha256, digest);
        QCOMPARE(Updates::checksumFor(QByteArray(digest.toLatin1() + "  CMRemote-0.3.0-windows-x64-setup.exe\n"), "CMRemote-0.3.0-windows-x64-setup.exe"), digest);
        QVERIFY(Updates::checksumFor("bad  CMRemote.exe\n", "CMRemote.exe").isEmpty());
        // A development build cannot replace itself.
        QVERIFY(!Updates::installerFor(withAssets->assets, "0.3.0", Updates::detectInstallation()));
    }
    void quickConnectPortFollowsProtocolDefault() {
        QCOMPARE(followDefaultPort("host:3389", "rdp", "ssh"), QString("host:22"));
        QCOMPARE(followDefaultPort("user@host:22", "ssh", "vnc"), QString("user@host:5900"));
        QCOMPARE(followDefaultPort("[::1]:5900", "vnc", "rdp"), QString("[::1]:3389"));
        QCOMPARE(followDefaultPort("host:8080", "rdp", "ssh"), QString("host:8080"));
        QCOMPARE(followDefaultPort("host", "rdp", "ssh"), QString("host"));
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.show(); WAIT_STORE(w);
        auto* combo = w.findChild<QComboBox*>("quickConnectCombo"); combo->setCurrentText("server:3389");
        w.setQuickConnectProtocol("ssh"); QCOMPARE(combo->currentText(), QString("server:22"));
        w.setQuickConnectProtocol("vnc"); QCOMPARE(combo->currentText(), QString("server:5900"));
    }
    void panelTabsHiddenByDefaultAndToggle() {
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.show(); WAIT_STORE(w);
        auto* action = w.findChild<QAction*>("actionViewPanels"); auto* groups = w.findChild<QTabWidget*>("groupTabs");
        QVERIFY(action && action->isEnabled() && !action->isChecked()); QVERIFY(groups->tabBar()->isHidden());
        action->trigger(); QVERIFY(!groups->tabBar()->isHidden());
        action->trigger(); QVERIFY(groups->tabBar()->isHidden());
        // Adding a panel shows the row so the new panel can be reached.
        w.addPanel(); QVERIFY(action->isChecked()); QVERIFY(!groups->tabBar()->isHidden());
    }
    void quickConnectSessionsOfferSaveBesideCopyPaste() {
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.show(); WAIT_STORE(w);
        w.setQuickConnectProtocol("ssh"); w.quickConnect("save.invalid", true);
        auto* view = w.findChild<TerminalView*>(); QVERIFY(view);
        QVERIFY(w.findChild<QAction*>("actionSessionSaveAsConnection"));
    }
    void updateCheckReachesGitHub() {
        // Contacts api.github.com; run with RM_NETWORK_TESTS=1.
        if (qEnvironmentVariable("RM_NETWORK_TESTS") != "1") QSKIP("Set RM_NETWORK_TESTS=1 to contact GitHub");
        UpdateChecker checker; QSignalSpy done(&checker, &UpdateChecker::finished); checker.check();
        QVERIFY(done.wait(20000));
        const bool ok = done.first().at(0).toBool(); const auto error = done.first().at(2).toString();
        // Before the first release GitHub answers 404, which still proves HTTPS works.
        QVERIFY2(ok || error == "No release has been published yet.", qPrintable(error));
        if (ok) QVERIFY(Updates::compareVersions(done.first().at(1).value<Updates::Release>().version, "0.0.0").has_value());
    }
    void checkForUpdatesReportsNewerReleaseAndLatest() {
        QTemporaryDir dir; MainWindow w(dir.filePath("confCons.xml"), true); w.show(); WAIT_STORE(w);
        auto* action = w.findChild<QAction*>("actionCheckUpdates"); QVERIFY(action->isVisible() && action->isEnabled());
        const auto answer = [&](const QString& name, const QString& tag) {
            QFile file(dir.filePath(name)); if (!file.open(QIODevice::WriteOnly)) return QUrl();
            file.write(QString("{\"tag_name\":\"%1\",\"html_url\":\"https://github.com/Crayjones/CMRemote/releases/tag/%1\"}").arg(tag).toUtf8());
            return QUrl::fromLocalFile(file.fileName());
        };
        const auto dialogText = [&] {
            QMessageBox* dialog = nullptr; if (!QTest::qWaitFor([&] { return (dialog = w.findChild<QMessageBox*>("updateDialog")); }, 5000)) return QString();
            QTextDocument text; text.setHtml(dialog->text()); const auto result = text.toPlainText() + "|" + dialog->text(); dialog->close(); delete dialog; return result;
        };
        w.setUpdateSource(answer("newer.json", "v999.0.0")); action->trigger();
        auto text = dialogText(); QVERIFY2(text.startsWith("CMRemote 999.0.0 is available. You have " + Updates::currentVersion()), qPrintable(text));
        QVERIFY(text.contains("href=\"https://github.com/Crayjones/CMRemote/releases/tag/v999.0.0\""));
        w.setUpdateSource(answer("same.json", "v" + Updates::currentVersion())); action->trigger();
        text = dialogText(); QVERIFY2(text.startsWith("You have the latest version, CMRemote " + Updates::currentVersion() + "."), qPrintable(text));
        w.setUpdateSource(QUrl::fromLocalFile(dir.filePath("missing.json"))); action->trigger();
        text = dialogText(); QVERIFY2(text.startsWith("Could not check for updates:"), qPrintable(text));
        QVERIFY(text.contains("href=\"https://github.com/Crayjones/CMRemote/releases\""));
    }
    void unprovidedFeaturesAreHiddenAndReconnectAllWorks() {
        QTemporaryDir dir; MainWindow w(dir.filePath("confCons.xml"), true); w.show(); WAIT_STORE(w);
        for (const auto* name : {"actionViewScreenshots", "actionHelpContents", "actionWebsite", "actionDonate"})
            QVERIFY(!w.findChild<QAction*>(name)->isVisible());
        auto* reconnect = w.findChild<QAction*>("actionReconnectAll"); QVERIFY(!reconnect->isEnabled());
        w.quickConnect("first.invalid", true); w.quickConnect("second.invalid", true); QVERIFY(reconnect->isEnabled());
        QStringList before; for (auto* pane : w.findChildren<SessionPane*>()) before << pane->id();
        reconnect->trigger();
        QTRY_VERIFY([&] { for (auto* pane : w.findChildren<SessionPane*>()) if (before.contains(pane->id())) return false; return true; }());
        QCOMPARE(w.sessionCount(), size_t(2));
    }
    void nativePreferencesAndConfigAreIndependent() {
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() != "cocoa") QSKIP("AppKit requires Cocoa");
        QTemporaryDir dir; MainWindow w(dir.filePath("profiles.json"), true); w.show(); WAIT_STORE(w);
        auto* options = w.findChild<QAction*>("actionOptions"); QTRY_VERIFY(options->isEnabled());
        auto* config = w.findChild<QAction*>("actionViewConfig"); auto* dock = w.findChild<QDockWidget*>("configDock");
        QSignalSpy opened(options, &QAction::triggered); QSignalSpy toggled(config, &QAction::triggered);
        dock->hide(); QTRY_VERIFY(!config->isChecked());
        activateNativeTestApplication(&w); w.raise(); w.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&w));
        QTRY_VERIFY(nativeMenuTitles().contains("View")); QVERIFY(dock->isHidden());
        QVERIFY(triggerNativePreferencesShortcut()); QTRY_VERIFY(opened.count() + toggled.count() > 0);
        QCOMPARE(toggled.count(), 0); QCOMPARE(opened.count(), 1);
        QTRY_VERIFY(w.findChild<QDialog*>("optionsDialog")); QVERIFY(dock->isHidden()); QVERIFY(!config->isChecked());
        w.findChild<QDialog*>("optionsDialog")->reject(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(nativeMenuTitles("View").contains("Config"));
        QVERIFY(triggerNativeMenuItem("View", "Config")); QTRY_VERIFY(dock->isVisible());
        QVERIFY(triggerNativeMenuItem("View", "Config")); QTRY_VERIFY(dock->isHidden());
        QCOMPARE(toggled.count(), 2); QCOMPARE(opened.count(), 1); QVERIFY(!w.findChild<QDialog*>("optionsDialog"));
        const auto appMenu = nativeMenuTitles().first(); const auto items = nativeMenuTitles(appMenu);
        QString preferences;
        for (const auto& title : items) if (title.startsWith("Preferences")) { QVERIFY(preferences.isEmpty()); preferences = title; }
        QVERIFY(!preferences.isEmpty()); QVERIFY(triggerNativeMenuItem(appMenu, preferences));
        QTRY_COMPARE(opened.count(), 2); QCOMPARE(toggled.count(), 2); QTRY_VERIFY(w.findChild<QDialog*>("optionsDialog"));
        w.findChild<QDialog*>("optionsDialog")->reject();
        QCOMPARE(config->menuRole(), QAction::NoRole); QCOMPARE(options->menuRole(), QAction::PreferencesRole);
#else
        QSKIP("AppKit requires macOS");
#endif
    }
    void nativeMenusExportRevisedActions() {
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() != "cocoa") QSKIP("AppKit requires Cocoa");
        QTemporaryDir dir; MainWindow w(dir.filePath("confCons.xml"), true); w.show(); WAIT_STORE(w);
        activateNativeTestApplication(&w); w.raise(); w.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&w));
        QTRY_VERIFY(nativeMenuTitles("View").contains("Jump To"));
        QTRY_VERIFY(nativeMenuTitles().contains("File")); QVERIFY(!nativeMenuTitles().contains("Sessions"));
        // Help holds only About, which macOS moves to the application menu.
        for (const auto* title : {"View", "Tools"}) QVERIFY(nativeMenuTitles().contains(title));
        QVERIFY(!nativeMenuTitles().contains("Help"));
        QTRY_VERIFY(nativeApplicationMenuTitles().contains("Check for Updates…"));
        const auto view = nativeMenuTitles("View"); QVERIFY(view.contains("Config")); QVERIFY(view.contains("Jump To")); QVERIFY(!view.contains("Screenshots")); QCOMPARE(view.first(), QString("Add Connection Panel"));
        const auto file = nativeMenuTitles("File");
        for (const auto* title : {"Open connection file…", "Save connection file", "Save connection file as…", "Save as connection…"}) QVERIFY(file.contains(title));
        QVERIFY(nativeMenuTitles("Tools").contains(w.findChild<QAction*>("actionDifferentCredentials")->text()));
#else
        QSKIP("AppKit requires macOS");
#endif
    }
    void designerShellAndNativeMenus() {
        const QString base = qEnvironmentVariable("RM_BUILD_DIR") + "/test-data";
        QDir().mkpath(base); QTemporaryDir dir(base + "/shell-XXXXXX"); QVERIFY(dir.isValid());
        MainWindow w(dir.path() + "/confCons.xml", true); w.show(); WAIT_STORE(w);
        QVERIFY(w.findChild<QDockWidget*>("connectionsDock"));
        QVERIFY(w.findChild<QDockWidget*>("configDock"));
        QCOMPARE(w.findChild<QAction*>("actionViewConfig")->menuRole(), QAction::NoRole);
        QCOMPARE(w.findChild<QAction*>("actionOptions")->menuRole(), QAction::PreferencesRole);
        QVERIFY(w.findChild<QDockWidget*>("notificationsDock"));
        auto* groups = w.findChild<QTabWidget*>("groupTabs"); QVERIFY(groups); QCOMPARE(groups->count(), 1);
        QCOMPARE(w.findChild<QTabWidget*>("sessionTabs")->count(), 0);
        QCOMPARE(w.findChild<QTreeWidget*>("connectionTree")->topLevelItemCount(), 1);
        QCOMPARE(w.sessionCount(), size_t(0)); QVERIFY(!QFile::exists(dir.path() + "/confCons.xml"));
        auto* config = w.findChild<QTreeWidget*>("configTree");
        for (int row = 0; row < config->topLevelItemCount(); ++row)
            QVERIFY(!config->itemWidget(config->topLevelItem(row), 0));
        QCOMPARE(w.windowTitle(), QString("CMRemote"));
        QCOMPARE(w.windowIcon().pixmap(QSize(32, 32), 1.0).toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied),
                 QImage(":/branding/cmremote-32.png").convertToFormat(QImage::Format_ARGB32_Premultiplied));
        for (const auto* name : {"actionAddFolder", "actionAddConnection", "actionTabReconnect"})
            QVERIFY(!w.findChild<QAction*>(name)->icon().pixmap(16).isNull());
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() == "cocoa") {
            activateNativeTestApplication(&w); w.raise(); w.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&w));
            QTRY_VERIFY(nativeMenuTitles("View").contains("Connections"));
            QTRY_VERIFY(nativeMenuTitles().contains("File"));
            for (const auto* title : {"View", "Tools"}) QVERIFY(nativeMenuTitles().contains(title));
            auto* dock = w.findChild<QDockWidget*>("connectionsDock"); QTRY_VERIFY(dock->isVisible());
            QVERIFY(triggerNativeMenuItem("View", "Connections")); QTRY_VERIFY(!dock->isVisible());
            QVERIFY(triggerNativeMenuItem("View", "Connections")); QTRY_VERIFY(dock->isVisible());
        }
#endif
    }
    void profileCrudInheritanceRestartAndTabLifecycle() {
        const QString base = qEnvironmentVariable("RM_BUILD_DIR");
        QVERIFY2(!base.isEmpty(), "Run tests through scripts/env.py");
        QDir().mkpath(base + "/test-data");
        QTemporaryDir dir(base + "/test-data/qt-XXXXXX"); QVERIFY(dir.isValid());
        const QString path = dir.path() + "/confCons.xml";
        QString childId;
        {
            MainWindow w(path,true); w.show(); WAIT_STORE(w);
            WAIT_STORE(w); w.addFolder(); WAIT_STORE(w);
            setGridText(w, "name", "Team"); WAIT_STORE(w);
            setGridText(w, "username", "alice"); WAIT_STORE(w);
            w.addConnection(); WAIT_STORE(w); childId = w.selectedId(); setGridText(w, "name", "Server"); WAIT_STORE(w);
            setGridText(w, "host", "127.0.0.1"); WAIT_STORE(w);
            QCOMPARE(gridValue(w, "username").toString(), QString("alice")); QVERIFY(!gridLocal(w, "username"));
            auto* tree = w.findChild<QTreeWidget*>("connectionTree");
            auto* tabs = w.findChild<QTabWidget*>("sessionTabs");
            QTest::qWait(50);
            const QPoint target = tree->visualItemRect(tree->currentItem()).center();
            QVERIFY(tree->viewport()->rect().contains(target));
            QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,target);
            w.connectSelected(true);
            QTRY_COMPARE(tabs->count(),1); QVERIFY(tabs->tabText(0).startsWith("MOCK"));
            w.connectSelected(true); QCOMPARE(tabs->count(),1); QCOMPARE(w.sessionCount(),size_t(1));
            auto* desktop = tabs->widget(0)->findChild<DesktopView*>(); QVERIFY(desktop);
            desktop->setFocus(); QTest::keyClick(desktop,Qt::Key_A);
            QTRY_VERIFY_WITH_TIMEOUT(tabs->widget(0)->findChild<QLabel*>("sessionStatus")->text().contains("Key"), 15000);
            w.resize(1000,700); QTest::qWait(80);
            QTRY_VERIFY(tabs->widget(0)->findChild<QLabel*>("sessionStatus")->text().contains("viewport"));
            w.deleteSelected(); WAIT_STORE(w); QVERIFY(w.lastError().contains("Close"));
            w.closeTab(0); QCOMPARE(w.sessionCount(),size_t(0));
            w.connectSelected(true); QCOMPARE(tabs->count(),1);
            w.disconnectCurrent(); QCOMPARE(w.sessionCount(),size_t(1));
            w.deleteSelected(); WAIT_STORE(w); QVERIFY(w.lastError().contains("Close"));
            w.reconnectCurrent(); QCOMPARE(tabs->count(),1); QCOMPARE(w.sessionCount(),size_t(1));
            QTest::qWait(50);
            QVERIFY(w.grab().save(base + "/qt-smoke.png"));
            w.closeTab(0);
        }
        {
            MainWindow w(path,true); WAIT_STORE(w);
            auto* tree = w.findChild<QTreeWidget*>("connectionTree");
            QCOMPARE(tree->topLevelItemCount(),1); QCOMPARE(tree->topLevelItem(0)->child(0)->text(0),QString("Team"));
            auto* child = tree->topLevelItem(0)->child(0)->child(0); QVERIFY(child);
            QCOMPARE(treeProfileId(child),childId); tree->setCurrentItem(child);
            QCOMPARE(gridValue(w, "host").toString(), QString("127.0.0.1"));
            w.deleteSelected(); WAIT_STORE(w); QCOMPARE(tree->topLevelItem(0)->child(0)->childCount(),0);
        }
    }
    void activeEditorValueReachesDirtyGuard() {
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/editor-XXXXXX"); QVERIFY(dir.isValid());
        const auto path = dir.path() + "/confCons.xml"; MainWindow w(path, true); w.show(); WAIT_STORE(w); w.addConnection(); WAIT_STORE(w);
        const auto id = w.selectedId(); setGridText(w, "host", "127.0.0.1"); WAIT_STORE(w);
        auto* port = qobject_cast<QSpinBox*>(gridEditor(w, "port")); QVERIFY(port);
        auto* line = port->findChild<QLineEdit*>(); line->selectAll(); QTest::keyClicks(line, "3391");
        w.connectSelected(true); QCOMPARE(w.sessionCount(), size_t(0));
        QVERIFY(!w.findChild<QMessageBox*>("unsavedChanges"));
        QTRY_COMPARE(w.sessionCount(), size_t(1));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto profiles = storedXml(path).value("profiles").toArray();
        QCOMPARE(profiles.at(0).toObject().value("id").toString(), id);
        const auto overrides = profiles.at(0).toObject().value("overrides").toObject();
        QCOMPARE(overrides.value("host").toString(), QString("127.0.0.1")); QCOMPARE(overrides.value("port").toInt(), 3391);
    }
    void failedSaveRetryRevertCancel() {
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/guards-XXXXXX"); QVERIFY(dir.isValid());
        MainWindow w(dir.path() + "/confCons.xml", true); w.show(); WAIT_STORE(w); w.addConnection(); WAIT_STORE(w); const auto first = w.selectedId();
        w.addConnection(); WAIT_STORE(w); const auto second = w.selectedId();
        auto* tree = w.findChild<QTreeWidget*>("connectionTree");
        const auto item = [&](const QString& id) -> QTreeWidgetItem* {
            for (QTreeWidgetItemIterator it(tree); *it; ++it) if (treeProfileId(*it) == id) return *it;
            return nullptr;
        };
        QVERIFY(item(first)); QVERIFY(item(second));
        QVERIFY(setDocumentFault(w, AtomicFile::Fault::BeforeReplace));
        setGridText(w, "name", "Pending"); QTRY_VERIFY(!w.lastError().isEmpty());
        auto choose = [&](QMessageBox::StandardButton answer) {
            QTRY_VERIFY(w.findChild<QMessageBox*>("saveRecovery")); auto* prompt = w.findChild<QMessageBox*>("saveRecovery");
            QCOMPARE(prompt->defaultButton(), prompt->button(QMessageBox::Cancel)); prompt->button(answer)->click();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        };
        tree->setCurrentItem(item(first)); choose(QMessageBox::Cancel);
        QCOMPARE(w.selectedId(), second); QCOMPARE(gridValue(w, "name").toString(), QString("Pending"));
        QVERIFY(setDocumentFault(w, AtomicFile::Fault::None));
        tree->setCurrentItem(item(first)); choose(QMessageBox::Retry); QTRY_COMPARE(w.selectedId(), first); WAIT_STORE(w);
        tree->setCurrentItem(item(second)); QCOMPARE(gridValue(w, "name").toString(), QString("Pending"));
        QVERIFY(setDocumentFault(w, AtomicFile::Fault::BeforeReplace));
        setGridText(w, "name", "Keep pending"); QTRY_VERIFY(!w.lastError().isEmpty());
        w.addFolder(); choose(QMessageBox::Cancel); QCOMPARE(tree->topLevelItem(0)->childCount(), 2);
        w.deleteSelected(); choose(QMessageBox::Cancel); QCOMPARE(tree->topLevelItem(0)->childCount(), 2);
        w.connectSelected(true); choose(QMessageBox::Cancel); QCOMPARE(w.sessionCount(), size_t(0));
        w.close(); choose(QMessageBox::Cancel); QVERIFY(w.isVisible()); QCOMPARE(gridValue(w, "name").toString(), QString("Keep pending"));
        w.close(); choose(QMessageBox::Discard); QVERIFY(!w.isVisible());
    }
    void failedSaveBlocksPendingAction() {
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/fail-save-XXXXXX"); QVERIFY(dir.isValid());
        const auto path = dir.path() + "/confCons.xml"; MainWindow w(path, true); w.show(); WAIT_STORE(w); w.addConnection(); WAIT_STORE(w);
        const auto id = w.selectedId(); setGridText(w, "name", ""); w.connectSelected(true);
        QTRY_VERIFY(!w.lastError().isEmpty());
        QCOMPARE(w.sessionCount(), size_t(0)); QCOMPARE(w.selectedId(), id); QVERIFY(gridValue(w, "name").toString().isEmpty());
        const auto validationError = w.lastError();
        QVERIFY(QFile::rename(path, path + ".before")); QVERIFY(QDir().mkdir(path));
        setGridText(w, "name", "Unsaved after disk failure"); w.close();
        QTRY_VERIFY(!w.lastError().isEmpty() && w.lastError() != validationError); QTRY_VERIFY(!documentBusy(w));
        QVERIFY(QFileInfo(path).isDir()); QVERIFY(w.isVisible()); QCOMPARE(gridValue(w, "name").toString(), QString("Unsaved after disk failure")); QCOMPARE(w.selectedId(), id);
        QVERIFY(QDir().rmdir(path)); QVERIFY(QFile::rename(path + ".before", path));
        w.close(); QTRY_VERIFY(w.findChild<QMessageBox*>("saveRecovery")); w.findChild<QMessageBox*>("saveRecovery")->button(QMessageBox::Discard)->click();
        QTRY_VERIFY(!w.isVisible()); QCOMPARE(gridValue(w, "name").toString(), QString("New connection"));
    }
    void explicitEmptyOverrideSurvivesRestart() {
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/empty-override-XXXXXX"); QVERIFY(dir.isValid());
        const auto path = dir.path() + "/confCons.xml";
        {
            MainWindow w(path, true); w.show(); WAIT_STORE(w); w.addFolder(); WAIT_STORE(w); setGridText(w, "name", "Parent"); WAIT_STORE(w);
            setGridText(w, "username", "inherited"); WAIT_STORE(w);
            w.addConnection(); WAIT_STORE(w); setGridText(w, "username", ""); WAIT_STORE(w);
        }
        MainWindow w(path, true); WAIT_STORE(w); auto* tree = w.findChild<QTreeWidget*>("connectionTree"); tree->setCurrentItem(tree->topLevelItem(0)->child(0)->child(0));
        QVERIFY(gridLocal(w, "username")); QVERIFY(gridValue(w, "username").toString().isEmpty());
        setGridLocal(w, "username", false);
        QCOMPARE(gridValue(w, "username").toString(), QString("inherited"));
    }
    void nativeQuitHonorsDirtyGuard() {
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/quit-XXXXXX"); QVERIFY(dir.isValid());
        MainWindow w(dir.path() + "/confCons.xml", true); w.show(); WAIT_STORE(w); w.addConnection(); WAIT_STORE(w);
        auto* name = qobject_cast<QLineEdit*>(gridEditor(w, "name")); name->selectAll(); QTest::keyClick(name, Qt::Key_Backspace);
        auto quit = [&] {
#ifdef Q_OS_MACOS
            if (QGuiApplication::platformName() == "cocoa") {
                const auto menus = nativeMenuTitles(); QVERIFY(!menus.isEmpty());
                const auto items = nativeMenuTitles(menus.first());
                const auto found = std::find_if(items.begin(), items.end(), [](const QString& text) { return text.startsWith("Quit "); });
                QVERIFY(found != items.end()); QVERIFY(triggerNativeMenuItem(menus.first(), *found)); return;
            }
#endif
            w.findChild<QAction*>("actionExit")->trigger();
        };
        quit(); QTRY_VERIFY(!w.lastError().isEmpty()); QVERIFY(w.isVisible()); QVERIFY(gridValue(w, "name").toString().isEmpty());
        quit(); QTRY_VERIFY(w.findChild<QMessageBox*>("saveRecovery")); auto* prompt = w.findChild<QMessageBox*>("saveRecovery");
        prompt->button(QMessageBox::Cancel)->click(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(w.isVisible());
        name = qobject_cast<QLineEdit*>(gridEditor(w, "name")); QTest::keyClicks(name, "Saved at Quit"); quit();
        QTRY_VERIFY(!w.isVisible()); QFile file(dir.filePath("confCons.xml")); QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(file.readAll().contains("Saved at Quit"));
    }
    void durabilityWarningBlocksPendingActionUntilAcknowledged() {
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/warning-XXXXXX"); QVERIFY(dir.isValid());
        const auto path = dir.path() + "/confCons.xml"; MainWindow w(path, true); w.show(); WAIT_STORE(w); w.addConnection(); WAIT_STORE(w); const auto first = w.selectedId();
        WAIT_STORE(w); w.addConnection(); WAIT_STORE(w); const auto second = w.selectedId(); auto* tree = w.findChild<QTreeWidget*>("connectionTree");
        auto saveAndWarn = [&] {
            QTRY_VERIFY(w.findChild<QMessageBox*>("durabilityWarning")); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        };
        QVERIFY(setDocumentFault(w, AtomicFile::Fault::DirectorySync)); setGridText(w, "name", "Committed warning");
        QTreeWidgetItem* firstItem = nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) if (treeProfileId(*it) == first) firstItem = *it;
        QVERIFY(firstItem); tree->setCurrentItem(firstItem); saveAndWarn();
        QCOMPARE(w.selectedId(), second); QVERIFY(!w.findChild<ProfileEditor*>()->dirty());
        auto* warning = w.findChild<QMessageBox*>("durabilityWarning"); QVERIFY(warning); QVERIFY(w.lastError().isEmpty());
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(file.readAll().contains("Committed warning")); file.close();
        warning->button(QMessageBox::Ok)->click(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QCOMPARE(w.selectedId(), first);
        QVERIFY(setDocumentFault(w, AtomicFile::Fault::DirectorySync)); setGridText(w, "name", "Before connect"); w.connectSelected(true); saveAndWarn();
        QCOMPARE(w.sessionCount(), size_t(0)); warning = w.findChild<QMessageBox*>("durabilityWarning"); QVERIFY(warning);
        warning->button(QMessageBox::Ok)->click(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QCOMPARE(w.sessionCount(), size_t(1));
        QVERIFY(setDocumentFault(w, AtomicFile::Fault::DirectorySync)); setGridText(w, "name", "Before close"); w.close(); saveAndWarn();
        QVERIFY(w.isVisible()); QCOMPARE(w.sessionCount(), size_t(1)); warning = w.findChild<QMessageBox*>("durabilityWarning"); QVERIFY(warning);
        warning->button(QMessageBox::Ok)->click(); QTRY_VERIFY(!w.isVisible()); QCOMPARE(w.sessionCount(), size_t(0));
    }
    void credentialCancelLeavesNoSession() {
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/cancel-credential-XXXXXX"); QVERIFY(dir.isValid());
        MainWindow w(dir.path() + "/confCons.xml", true); w.show(); WAIT_STORE(w); w.addConnection(); WAIT_STORE(w);
        setGridText(w, "host", "127.0.0.1"); WAIT_STORE(w);
#ifdef RM_WITH_RDP
        w.connectSelected(false); QTRY_VERIFY(w.findChild<QDialog*>("CredentialsDialog")); auto* dialog = w.findChild<QDialog*>("CredentialsDialog");
        QCOMPARE(w.sessionCount(), size_t(0)); QCOMPARE(w.findChild<QTabWidget*>("sessionTabs")->count(), 0);
        dialog->reject(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(w.sessionCount(), size_t(0)); QCOMPARE(w.findChild<QTabWidget*>("sessionTabs")->count(), 0);
        w.quickConnect("127.0.0.1:3389", false); dialog = w.findChild<QDialog*>("CredentialsDialog"); QVERIFY(dialog);
        QPointer<QDialog> pending = dialog; w.close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!pending); QCOMPARE(w.sessionCount(), size_t(0)); QCOMPARE(w.findChild<QTabWidget*>("sessionTabs")->count(), 0);
#else
        w.connectSelected(false); QCOMPARE(w.sessionCount(), size_t(0)); QVERIFY(!w.findChild<QDialog*>("CredentialsDialog"));
#endif
    }
    void temporaryReconnectPreservesTarget() {
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/temporary-XXXXXX"); QVERIFY(dir.isValid());
        const auto path = dir.path() + "/confCons.xml"; MainWindow w(path, true); w.show(); WAIT_STORE(w);
        w.quickConnect("[2001:0db8::1]:3391", true); auto* tabs = w.findChild<QTabWidget*>("sessionTabs"); QCOMPARE(tabs->count(), 1);
        auto* pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane); QVERIFY(pane->profile().isEmpty());
        const auto target = pane->target(); const auto old = pane->id(); const auto caption = tabs->tabText(0);
        QCOMPARE(target.host, QString("2001:db8::1")); QCOMPARE(target.port, quint16(3391));
        w.quickConnect("[2001:db8::1]:3391", true); QCOMPARE(tabs->count(), 1);
        w.disconnectCurrent(); QCOMPARE(tabs->count(), 1); QCOMPARE(w.sessionCount(), size_t(1)); QVERIFY(!pane->active()); QVERIFY(!QFile::exists(path));
        w.reconnectCurrent(); QCOMPARE(tabs->count(), 1); QCOMPARE(w.sessionCount(), size_t(1));
        pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane->id() != old); QVERIFY(pane->target() == target); QCOMPARE(tabs->tabText(0), caption);
        w.saveAsConnection(); auto* dialog = w.findChild<QDialog*>("saveAsConnection"); QVERIFY(dialog);
        dialog->findChild<QLineEdit*>("connectionName")->setText("Explicitly saved"); dialog->accept(); WAIT_STORE(w);
        QCOMPARE(tabs->count(), 1); QVERIFY(pane->profile().isEmpty()); QVERIFY(pane->target() == target);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto bytes = file.readAll(); file.close();
        w.connectSelected(true); QCOMPARE(tabs->count(), 2); QVERIFY(!qobject_cast<SessionPane*>(tabs->widget(1))->profile().isEmpty());
        tabs->setCurrentIndex(0); w.disconnectCurrent(); w.reconnectCurrent(); w.closeTab(0); w.closeTab(0);
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), bytes);
    }
    void profileEditsDoNotRetargetRunningSession() {
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/snapshot-XXXXXX"); QVERIFY(dir.isValid());
        MainWindow w(dir.path() + "/confCons.xml", true); w.show(); WAIT_STORE(w); w.addConnection(); WAIT_STORE(w);
        setGridText(w, "host", "127.0.0.1"); WAIT_STORE(w);
        setGridText(w, "username", "alice"); WAIT_STORE(w); setGridText(w, "domain", "EXAMPLE"); WAIT_STORE(w);
        setGridChoice(w, "resolution", "1280x720"); WAIT_STORE(w); setGridChoice(w, "automatic_resize", false); WAIT_STORE(w);
        w.connectSelected(true); auto* tabs = w.findChild<QTabWidget*>("sessionTabs"); auto* pane = qobject_cast<SessionPane*>(tabs->widget(0));
        const auto old = pane->id(); const auto captured = pane->target();
        QCOMPARE(captured.domain, QString("EXAMPLE")); QCOMPARE(captured.options.resolution, QString("1280x720")); QVERIFY(!captured.options.automaticResize);
        setGridText(w, "host", "changed.invalid"); WAIT_STORE(w); setGridText(w, "domain", "OTHER"); WAIT_STORE(w);
        setGridChoice(w, "resolution", "800x600"); WAIT_STORE(w); setGridChoice(w, "automatic_resize", true); WAIT_STORE(w);
        setGridChoice(w, "color_depth", 16); WAIT_STORE(w); setGridChoice(w, "cache_bitmaps", true); WAIT_STORE(w);
        QVERIFY(pane->target() == captured); w.reconnectCurrent();
        pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane->id() != old); QCOMPARE(pane->target().host, QString("changed.invalid"));
        QCOMPARE(pane->target().domain, QString("OTHER")); QCOMPARE(pane->target().options.resolution, QString("800x600"));
        QVERIFY(pane->target().options.automaticResize); QCOMPARE(pane->target().options.colorDepth, quint8(16)); QVERIFY(pane->target().options.cacheBitmaps);
    }
    void temporarySaveAsPreservesCapturedOptions() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); MainWindow w(path, true); w.show(); WAIT_STORE(w);
        w.addFolder(); WAIT_STORE(w); const auto folder = w.selectedId();
        setGridText(w, "username", "inherited-user"); WAIT_STORE(w); setGridText(w, "domain", "INHERITED"); WAIT_STORE(w);
        setGridChoice(w, "resolution", "1920x1080"); WAIT_STORE(w); setGridChoice(w, "automatic_resize", false); WAIT_STORE(w);
        setGridChoice(w, "color_depth", 16); WAIT_STORE(w); setGridChoice(w, "cache_bitmaps", true); WAIT_STORE(w);
        w.quickConnect("localhost:3391", true); auto* tabs = w.findChild<QTabWidget*>("sessionTabs"); auto* pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane);
        const auto captured = pane->target(); w.saveAsConnection(); auto* save = w.findChild<QDialog*>("saveAsConnection"); QVERIFY(save);
        auto* parent = save->findChild<QComboBox*>("connectionParent");
        const auto folderIndex = parent->findData(QVariant::fromValue(ProfileReference{w.selectedDocument(), folder})); QVERIFY(folderIndex >= 0);
        parent->setCurrentIndex(folderIndex); save->accept(); WAIT_STORE(w);
        const auto profiles = storedXml(path).value("profiles").toArray(); QCOMPARE(profiles.size(), 2);
        QJsonObject saved;
        for (const auto& value : profiles) if (value.toObject()["id"].toString() == w.selectedId()) saved = value.toObject();
        QVERIFY(!saved.isEmpty()); QCOMPARE(saved["parent"].toString(), folder);
        const auto overrides = saved.value("overrides").toObject(), expected = captured.overrides();
        for (auto it = expected.begin(); it != expected.end(); ++it) QCOMPARE(overrides.value(it.key()), it.value());
        QVERIFY(pane->profile().isEmpty()); QVERIFY(pane->target() == captured);
        w.connectSelected(true); QCOMPARE(tabs->count(), 2); QVERIFY(qobject_cast<SessionPane*>(tabs->widget(1))->target() == captured);
    }
    void savedSshMockTerminalLifecycle() {
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/ssh-mock-XXXXXX"); QVERIFY(dir.isValid());
        MainWindow window(dir.path() + "/confCons.xml", true); window.show(); WAIT_STORE(window);
        window.addConnection(); WAIT_STORE(window); const auto profile = window.selectedId();
        setGridChoice(window, "protocol", "ssh"); WAIT_STORE(window);
        setGridText(window, "host", "localhost"); WAIT_STORE(window);
        setGridPort(window, 22); WAIT_STORE(window);
        window.connectSelected(true);
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); QTRY_COMPARE(tabs->count(), 1);
        auto* pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane); QCOMPARE(pane->profile(), profile);
        QVERIFY(tabs->tabText(0).startsWith("MOCK")); QVERIFY(!pane->findChild<DesktopView*>());
        auto* terminal = pane->findChild<TerminalView*>(); QVERIFY(terminal);
        auto* status = pane->findChild<QLabel*>("sessionStatus"); QVERIFY(status);
        const auto previous = pane->id(); window.connectSelected(true); QCOMPARE(tabs->count(), 1); QCOMPARE(pane->id(), previous);
        terminal->setFocus(); QTest::keyClick(terminal, Qt::Key_A); QTRY_VERIFY(status->text().contains("Key"));
        window.resize(window.width() + 80, window.height() + 40); QTRY_VERIFY(status->text().contains("viewport"));
        window.disconnectCurrent(); QCOMPARE(tabs->count(), 1); QVERIFY(!pane->active()); QCOMPARE(window.sessionCount(), size_t(1));
        QTest::qWait(50); const auto disconnected = status->text();
        QTest::keyClick(terminal, Qt::Key_B); QTest::qWait(50); QCOMPARE(status->text(), disconnected);
        window.deleteSelected(); WAIT_STORE(window); QVERIFY(window.lastError().contains("Close all tabs")); QCOMPARE(window.selectedId(), profile);
        window.reconnectCurrent(); QCOMPARE(tabs->count(), 1);
        auto* fresh = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(fresh); QVERIFY(fresh->id() != previous); QVERIFY(fresh->active());
        QCOMPARE(fresh->profile(), profile); QVERIFY(fresh->findChild<TerminalView*>()); QVERIFY(!fresh->findChild<DesktopView*>());
        QPointer<SessionPane> closed = fresh; window.closeTab(0); QVERIFY(!closed); QCOMPARE(tabs->count(), 0); QCOMPARE(window.sessionCount(), size_t(0));
        window.deleteSelected(); WAIT_STORE(window); QVERIFY(window.selectedId().isEmpty());
    }
    void disconnectKeepsTabCloseRemovesIt() {
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/disconnect-XXXXXX"); QVERIFY(dir.isValid());
        MainWindow w(dir.path() + "/confCons.xml", true); w.show(); WAIT_STORE(w); w.resize(1100, 850); w.quickConnect("127.0.0.1:3392", true);
        auto* tabs = w.findChild<QTabWidget*>("sessionTabs"); QCOMPARE(tabs->count(), 1);
        auto* pane = qobject_cast<SessionPane*>(tabs->widget(0)); QVERIFY(pane);
        auto* status = pane->findChild<QLabel*>("sessionStatus"); auto* view = pane->findChild<DesktopView*>(); QVERIFY(status && view);
        QTRY_VERIFY(status->isVisible()); QTRY_VERIFY(status->height() <= status->sizeHint().height());
        QVERIFY(view->height() > pane->height() * 3 / 4);
        w.disconnectCurrent(); QCOMPARE(tabs->count(), 1); QCOMPARE(w.sessionCount(), size_t(1)); QVERIFY(!pane->active());
        QTRY_VERIFY(status->isVisible()); QTRY_VERIFY(status->height() <= status->sizeHint().height());
        w.closeTab(0); QCOMPARE(tabs->count(), 0); QCOMPARE(w.sessionCount(), size_t(0)); QVERIFY(!QFile::exists(dir.path() + "/confCons.xml"));
    }
    void clickedMovedTabTargetsCorrectSession() {
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/moved-tabs-XXXXXX"); QVERIFY(dir.isValid());
        MainWindow w(dir.path() + "/confCons.xml", true); w.show(); WAIT_STORE(w);
        w.quickConnect("first.invalid", true); w.quickConnect("second.invalid", true);
        auto* tabs = w.findChild<QTabWidget*>("sessionTabs"); auto* groups = w.findChild<QTabWidget*>("groupTabs");
        auto* first = qobject_cast<SessionPane*>(tabs->widget(0)); auto* second = qobject_cast<SessionPane*>(tabs->widget(1));
        auto context = [&](QTabWidget* owner, QWidget* page) {
            const auto point = owner->tabBar()->tabRect(owner->indexOf(page)).center();
            QContextMenuEvent event(QContextMenuEvent::Mouse, point, owner->tabBar()->mapToGlobal(point));
            QApplication::sendEvent(owner->tabBar(), &event);
        };
        context(tabs, first); auto* menu = w.findChild<QMenu*>("sessionContextMenu"); QVERIFY(menu);
        QVERIFY(!menu->findChild<QAction*>("actionTabDuplicate")); QVERIFY(!menu->findChild<QAction*>("actionTabSmartSize"));
        tabs->tabBar()->moveTab(0, 1); tabs->setCurrentWidget(second);
        menu->findChild<QAction*>("actionTabDisconnect")->trigger(); menu->close();
        QVERIFY(!first->active()); QVERIFY(second->active()); QCOMPARE(tabs->count(), 2);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        context(tabs, first); menu = w.findChild<QMenu*>("sessionContextMenu"); QVERIFY(menu);
        menu->findChild<QAction*>("actionTabRename")->trigger(); menu->close();
        auto* rename = w.findChild<QInputDialog*>("renamePage"); QVERIFY(rename); rename->setTextValue("Same caption"); rename->accept();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(tabs->tabText(tabs->indexOf(first)), QString("Same caption"));
        w.findChild<QAction*>("actionAddPanel")->trigger(); QCOMPARE(groups->count(), 2);
        auto* other = groups->currentWidget()->findChild<QTabWidget*>(); QVERIFY(other);
        groups->setCurrentIndex(0); context(tabs, first); menu = w.findChild<QMenu*>("sessionContextMenu"); QVERIFY(menu);
        auto* move = menu->findChild<QMenu*>("moveSessionMenu"); QVERIFY(move); QCOMPARE(move->actions().size(), 2);
        move->actions().at(1)->trigger(); menu->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(other->widget(0), first); QCOMPARE(tabs->count(), 1);
        groups->setCurrentIndex(0); w.quickConnect("FIRST.invalid", true);
        QCOMPARE(groups->currentIndex(), 1); QCOMPARE(other->widget(0), first); QCOMPARE(w.sessionCount(), size_t(2));
        QVERIFY(!first->active()); QVERIFY(second->active());
        const auto previous = first->id(); w.reconnectCurrent(); auto* fresh = qobject_cast<SessionPane*>(other->widget(0)); QVERIFY(fresh->id() != previous); QVERIFY(fresh->active());
        QCOMPARE(other->tabText(0), QString("Same caption"));
        w.resize(w.width() + 40, w.height() + 40);
        QTRY_VERIFY(fresh->findChild<QLabel*>("sessionStatus")->text().contains("viewport"));
        fresh->findChild<DesktopView*>()->setFocus(); QTest::keyClick(fresh->findChild<DesktopView*>(), Qt::Key_A);
        QTRY_VERIFY(fresh->findChild<QLabel*>("sessionStatus")->text().contains("Key"));
        context(other, fresh); menu = w.findChild<QMenu*>("sessionContextMenu"); QVERIFY(menu);
        QPointer<SessionPane> gone = fresh; menu->findChild<QAction*>("actionTabClose")->trigger(); menu->close();
        QVERIFY(!gone); QCOMPARE(other->count(), 0); QVERIFY(second->active());
        menu->findChild<QAction*>("actionTabClose")->trigger(); QCOMPARE(tabs->count(), 1);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(second->contextMenuPolicy(), Qt::PreventContextMenu);
    }
    void disconnectedTabsBlockSubtreeDelete() {
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/subtree-tabs-XXXXXX"); QVERIFY(dir.isValid());
        MainWindow w(dir.path() + "/confCons.xml", true); w.show(); WAIT_STORE(w); w.addFolder(); WAIT_STORE(w); const auto folder = w.selectedId();
        WAIT_STORE(w); w.addConnection(); WAIT_STORE(w); const auto child = w.selectedId(); w.connectSelected(true);
        auto* groups = w.findChild<QTabWidget*>("groupTabs"); auto* general = w.findChild<QTabWidget*>("sessionTabs");
        auto* pane = qobject_cast<SessionPane*>(general->widget(0)); const auto id = pane->id();
        w.findChild<QAction*>("actionAddPanel")->trigger(); QCOMPARE(groups->count(), 2);
        auto* other = groups->currentWidget()->findChild<QTabWidget*>(); QVERIFY(other);
        general->removeTab(0); other->addTab(pane, "Renamed");
        w.connectSelected(true); QCOMPARE(other->count(), 1); QCOMPARE(general->count(), 0); QCOMPARE(pane->id(), id);
        w.disconnectCurrent(); QCOMPARE(w.sessionCount(), size_t(1)); QVERIFY(!pane->active());
        auto* tree = w.findChild<QTreeWidget*>("connectionTree"); tree->setCurrentItem(tree->topLevelItem(0)->child(0)); QCOMPARE(w.selectedId(), folder);
        w.deleteSelected(); WAIT_STORE(w); QVERIFY(w.lastError().contains("Close all tabs")); QCOMPARE(tree->topLevelItemCount(), 1);
        tree->setCurrentItem(tree->topLevelItem(0)->child(0)->child(0)); QCOMPARE(w.selectedId(), child);
        groups->setCurrentIndex(0); w.connectSelected(true); QCOMPARE(groups->currentIndex(), 1); QCOMPARE(w.sessionCount(), size_t(1)); QVERIFY(!pane->active());
        w.closeTab(0); QCOMPARE(w.sessionCount(), size_t(0)); tree->setCurrentItem(tree->topLevelItem(0)->child(0)); w.deleteSelected(); WAIT_STORE(w); QCOMPARE(tree->topLevelItem(0)->childCount(), 0);
    }
    void panelCloseStopsAllWorkers() {
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/panel-close-XXXXXX"); QVERIFY(dir.isValid());
        MainWindow w(dir.path() + "/confCons.xml", true); w.show(); WAIT_STORE(w);
        auto* groups = w.findChild<QTabWidget*>("groupTabs");
        auto context = [&](QWidget* page) {
            const auto point = groups->tabBar()->tabRect(groups->indexOf(page)).center();
            QContextMenuEvent event(QContextMenuEvent::Mouse, point, groups->tabBar()->mapToGlobal(point)); QApplication::sendEvent(groups->tabBar(), &event);
        };
        auto* general = groups->widget(0); groups->setTabText(0, "Renamed General"); context(general);
        auto* menu = w.findChild<QMenu*>("groupContextMenu"); QVERIFY(menu); QVERIFY(!menu->findChild<QAction*>("actionPanelClose")->isEnabled());
        menu->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        w.findChild<QAction*>("actionAddPanel")->trigger(); QCOMPARE(groups->count(), 2);
        QPointer<QWidget> panel = groups->currentWidget(); w.quickConnect("first.invalid", true); w.quickConnect("second.invalid", true);
        const auto panes = panel->findChildren<SessionPane*>(); QCOMPARE(panes.size(), 2); QPointer<SessionPane> one = panes.at(0), two = panes.at(1);
        context(panel); menu = w.findChild<QMenu*>("groupContextMenu"); QVERIFY(menu); menu->findChild<QAction*>("actionPanelClose")->trigger(); menu->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(!one && !two && !panel); QCOMPARE(groups->count(), 1); QCOMPARE(w.sessionCount(), size_t(0));
#ifdef RM_WITH_RDP
        w.findChild<QAction*>("actionAddPanel")->trigger(); panel = groups->currentWidget(); w.quickConnect("127.0.0.1:1");
        QPointer<QDialog> credentials = w.findChild<QDialog*>("CredentialsDialog"); QVERIFY(credentials);
        context(panel); menu = w.findChild<QMenu*>("groupContextMenu"); QVERIFY(menu); menu->findChild<QAction*>("actionPanelClose")->trigger(); menu->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(!credentials); QCOMPARE(w.sessionCount(), size_t(0));
#endif
        w.quickConnect("general.invalid", true); w.findChild<QAction*>("actionAddPanel")->trigger(); w.quickConnect("other.invalid", true);
        QCOMPARE(w.sessionCount(), size_t(2)); w.close(); QCOMPARE(w.sessionCount(), size_t(0)); QVERIFY(w.findChildren<SessionPane*>().isEmpty());
    }
    void contextDraftGuardsAndSupportedCommands() {
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/context-guards-XXXXXX"); QVERIFY(dir.isValid());
        MainWindow w(dir.path() + "/confCons.xml", true); w.show(); WAIT_STORE(w); w.addConnection(); WAIT_STORE(w); const auto other = w.selectedId();
        w.addConnection(); WAIT_STORE(w); const auto current = w.selectedId(); QVERIFY(other != current);
        auto* tree = w.findChild<QTreeWidget*>("connectionTree"); auto* name = qobject_cast<QLineEdit*>(gridEditor(w, "name")); name->selectAll(); QTest::keyClick(name, Qt::Key_Backspace);
        auto context = [&] {
            QTreeWidgetItem* target = nullptr;
            for (QTreeWidgetItemIterator it(tree); *it; ++it) if (treeProfileId(*it) == other) target = *it;
            QVERIFY(target); const auto point = tree->visualItemRect(target).center();
            QContextMenuEvent event(QContextMenuEvent::Mouse, point, tree->viewport()->mapToGlobal(point)); QApplication::sendEvent(tree->viewport(), &event);
        };
        context(); QTRY_VERIFY(!w.lastError().isEmpty()); QCOMPARE(w.selectedId(), current); QVERIFY(!w.findChild<QMenu*>("treeContextMenu"));
        context(); QTRY_VERIFY(w.findChild<QMessageBox*>("saveRecovery")); auto* prompt = w.findChild<QMessageBox*>("saveRecovery"); prompt->button(QMessageBox::Cancel)->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QCOMPARE(w.selectedId(), current); QVERIFY(gridValue(w, "name").toString().isEmpty());
        QVERIFY(!w.findChild<QMenu*>("treeContextMenu"));
        context(); prompt = w.findChild<QMessageBox*>("saveRecovery"); QVERIFY(prompt); prompt->button(QMessageBox::Discard)->click();
        auto* menu = w.findChild<QMenu*>("treeContextMenu"); QVERIFY(menu); QCOMPARE(w.selectedId(), other);
        QVERIFY(menu->findChild<QAction*>("actionTreeDuplicate")->isEnabled()); QVERIFY(menu->findChild<QMenu*>("connectOptionsMenu")); menu->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        for (const auto* action : {"actionReconnectAll", "actionDefaultInheritance"}) QVERIFY(!w.findChild<QAction*>(action)->isEnabled());
        auto* dock = w.findChild<QDockWidget*>("configDock"); w.findChild<QAction*>("actionViewConfig")->trigger(); QVERIFY(!dock->isVisible());
        w.findChild<QAction*>("actionResetLayout")->trigger(); QVERIFY(dock->isVisible()); QVERIFY(w.findChild<QAction*>("actionViewConfig")->isChecked());
        w.deleteSelected(); WAIT_STORE(w); auto* list = w.findChild<QListWidget*>("notificationsList"); QVERIFY(list->count() > 0);
        list->clearSelection(); QContextMenuEvent event(QContextMenuEvent::Keyboard, {}, list->mapToGlobal(list->rect().center())); QApplication::sendEvent(list, &event);
        menu = w.findChild<QMenu*>("notificationContextMenu"); QVERIFY(menu); QVERIFY(menu->findChild<QAction*>("actionNotificationCopy")->isEnabled());
        menu->findChild<QAction*>("actionNotificationDelete")->trigger(); menu->close(); QCOMPARE(list->count(), 0);
    }
    void treeMenuMatchesMremotengAndCommandsWork() {
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/tree-menu-XXXXXX"); QVERIFY(dir.isValid());
        MainWindow w(dir.path() + "/confCons.xml", true); w.show(); WAIT_STORE(w);
        w.addConnection(); WAIT_STORE(w); const auto first = w.selectedId();
        w.addConnection(); WAIT_STORE(w); const auto second = w.selectedId();
        auto* tree = w.findChild<QTreeWidget*>("connectionTree");
        auto order = [&] { QStringList ids; auto* root = tree->topLevelItem(0); for (int i = 0; i < root->childCount(); ++i) ids << treeProfileId(root->child(i)); return ids; };
        QCOMPARE(order(), QStringList({first, second}));
        auto openMenu = [&](const QString& id) -> QMenu* {
            QTreeWidgetItem* target = nullptr; for (QTreeWidgetItemIterator it(tree); *it; ++it) if (treeProfileId(*it) == id) target = *it;
            const auto point = tree->visualItemRect(target).center();
            QContextMenuEvent event(QContextMenuEvent::Mouse, point, tree->viewport()->mapToGlobal(point)); QApplication::sendEvent(tree->viewport(), &event);
            return w.findChild<QMenu*>("treeContextMenu");
        };
        auto* menu = openMenu(second); QVERIFY(menu);
        QStringList labels; for (auto* action : menu->actions()) if (!action->isSeparator()) labels << action->text();
        QCOMPARE(labels, QStringList({"Connect", "Connect (with options)", "Disconnect", "External Tools", "Transfer File (SSH)", "Duplicate", "Rename", "Delete…",
                                      "Import", "Export to File…", "New Connection", "New folder", "Sort", "Move up", "Move down"}));
        QCOMPARE(menu->findChild<QAction*>("actionTreeDuplicate")->shortcut(), QKeySequence("Ctrl+D"));
        QVERIFY(menu->findChild<QAction*>("actionMoveUp")->isEnabled()); QVERIFY(!menu->findChild<QAction*>("actionMoveDown")->isEnabled());
        QVERIFY(!menu->findChild<QAction*>("actionPasswordEdit")); QVERIFY(!menu->findChild<QAction*>("actionEditPassword"));
        menu->findChild<QAction*>("actionMoveUp")->trigger(); menu->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        WAIT_STORE(w); QCOMPARE(order(), QStringList({second, first}));
        menu = openMenu(second); menu->findChild<QAction*>("actionTreeDuplicate")->trigger(); menu->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); WAIT_STORE(w);
        const auto ids = order(); QCOMPARE(ids.size(), 3); QCOMPARE(ids.at(0), second); QCOMPARE(ids.at(2), first);
        QVERIFY(ids.at(1) != second); QCOMPARE(w.selectedId(), ids.at(1));
        menu = openMenu(first); QVERIFY(!menu->findChild<QAction*>("actionTreeDisconnect")->isEnabled()); menu->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        w.connectSelected(true); QCOMPARE(w.sessionCount(), size_t(1));
        menu = openMenu(first); auto* disconnect = menu->findChild<QAction*>("actionTreeDisconnect"); QVERIFY(disconnect->isEnabled());
        disconnect->trigger(); menu->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(w.sessionCount(), size_t(0)); QVERIFY(w.findChildren<SessionPane*>().isEmpty());
    }
    void quickTargetParsing() {
        for (const auto* endpoint : {"example.invalid", "127.0.0.1:3391", "[::1]", "[2001:db8::1]:3392", "2001:db8::1"})
            QVERIFY(parseRdpTarget(endpoint).has_value());
        QCOMPARE(parseRdpTarget("EXAMPLE.invalid")->host, QString("example.invalid"));
        QCOMPARE(parseRdpTarget("2001:0db8::1")->host, QString("2001:db8::1"));
        for (const auto* endpoint : {"", "host:", "host:0", "host:65536", "host:-1", "host:abc", "https://host", "user@host", "host/path", "[::1", "[]", "[host]:3389", "host\n", "host name", "[::1]junk"})
            QVERIFY(!parseRdpTarget(endpoint).has_value());
    }
    void defaultDataFollowsConfigHomeAndMigratesLegacyDataOnce() {
        const QString base = qEnvironmentVariable("RM_BUILD_DIR") + "/test-data";
        QTemporaryDir root(base + "/migrate-XXXXXX"); QVERIFY(root.isValid());
#ifndef Q_OS_WIN
        const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
        qputenv("XDG_CONFIG_HOME", (root.path() + "/config").toUtf8());
        QCOMPARE(defaultDataDirectory(), root.path() + "/config/CMRemote");
        qputenv("XDG_CONFIG_HOME", "relative");
        QCOMPARE(defaultDataDirectory(), QDir::homePath() + "/.config/CMRemote");
        if (previous.isNull()) qunsetenv("XDG_CONFIG_HOME"); else qputenv("XDG_CONFIG_HOME", previous);
#endif
        const auto write = [](const QString& path, const QByteArray& bytes) {
            QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        const QString legacy = root.path() + "/legacy", target = root.path() + "/new";
        QVERIFY(migrateLegacyData(target, legacy)); QVERIFY(!QFileInfo::exists(target + "/confCons.xml"));
        QVERIFY(write(legacy + "/confCons.xml", "<old/>")); QVERIFY(write(legacy + "/confCons.xml.lock", "x"));
        QVERIFY(write(legacy + "/preferences.json", "{}")); QVERIFY(write(legacy + "/freerdp/server/known", "cert"));
        QVERIFY(write(target + "/preferences.json", "{\"kept\":1}"));
        QVERIFY(migrateLegacyData(target, legacy));
        const auto read = [](const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); };
        QCOMPARE(read(target + "/confCons.xml"), QByteArray("<old/>"));
        QCOMPARE(read(target + "/freerdp/server/known"), QByteArray("cert"));
        QCOMPARE(read(target + "/preferences.json"), QByteArray("{\"kept\":1}"));
        QVERIFY(!QFileInfo::exists(target + "/confCons.xml.lock")); QVERIFY(!QFileInfo::exists(target + "/confCons.xml.migrating"));
        QCOMPARE(read(legacy + "/confCons.xml"), QByteArray("<old/>"));
        // Once the new file exists, later launches never copy over it.
        QVERIFY(write(legacy + "/confCons.xml", "<newer-legacy/>")); QVERIFY(migrateLegacyData(target, legacy));
        QCOMPARE(read(target + "/confCons.xml"), QByteArray("<old/>"));
    }
    void containedDevelopmentPathsAndTransientCredentials() {
        const QString base = qEnvironmentVariable("RM_BUILD_DIR") + "/test-data";
        QTemporaryDir directory(base + "/paths-XXXXXX"); QVERIFY(directory.isValid());
        QFile marker(directory.path() + "/Cargo.toml"); QVERIFY(marker.open(QIODevice::WriteOnly)); marker.close();
        const QString path = applicationDataDirectory(directory.path());
        QVERIFY(path.startsWith(directory.path() + "/.dev/"));
        QCOMPARE(applicationDataDirectory(), defaultDataDirectory());
        QVERIFY(defaultDataDirectory().endsWith("/CMRemote"));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, applicationDataDirectory("relative"));
        PromptCredentialProvider provider; QWidget owner;
        std::optional<TransientCredentials> credentials; bool completed = false;
        auto* dialog = provider.request(&owner, {"test endpoint", "initial-user"}, [&](std::optional<TransientCredentials> result) { credentials = std::move(result); completed = true; });
        auto* password = dialog->findChild<QLineEdit*>("passwordEdit");
        QVERIFY(password->echoMode() == QLineEdit::Password); QVERIFY(!dialog->findChild<QCheckBox*>("clipboardCheck")->isChecked());
        QCOMPARE(dialog->findChild<QLineEdit*>("usernameEdit")->text(), QString("initial-user"));
        dialog->findChild<QLineEdit*>("usernameEdit")->setText("session-user"); password->setText("test-only-unsaved-secret"); dialog->accept();
        QVERIFY(completed); QVERIFY(credentials.has_value()); QVERIFY(!credentials->clipboard);
        QVERIFY(credentials->password == QByteArray("test-only-unsaved-secret")); QCOMPARE(credentials->username, QString("session-user")); QVERIFY(password->text().isEmpty());
        credentials.reset(); completed = false;
        dialog = provider.request(&owner, {"test endpoint", {}}, [&](std::optional<TransientCredentials> result) { credentials = std::move(result); completed = true; });
        dialog->reject(); QVERIFY(completed); QVERIFY(!credentials.has_value());
    }
    void rhiLetterboxVerticesMatchPresentation() {
        for (const auto sizes : {std::pair{QSize(900, 650), QSize(1280, 720)}, std::pair{QSize(1120, 810), QSize(800, 600)},
                                  std::pair{QSize(400, 300), QSize(400, 300)}, std::pair{QSize(640, 200), QSize(320, 240)}}) {
            QSize scaled = sizes.second; scaled.scale(sizes.first, Qt::KeepAspectRatio);
            const QRect expected(QPoint((sizes.first.width() - scaled.width()) / 2, (sizes.first.height() - scaled.height()) / 2), scaled);
            const auto v = DesktopView::letterboxVertices(sizes.first, sizes.second);
            const auto px = [&](float ndc, int extent) { return int((ndc + 1) / 2 * extent + 0.5f); };
            const auto py = [&](float ndc, int extent) { return int((1 - ndc) / 2 * extent + 0.5f); };
            QCOMPARE(px(v[0], sizes.first.width()), expected.left()); QCOMPARE(py(v[1], sizes.first.height()), expected.top());
            QCOMPARE(px(v[12], sizes.first.width()), expected.right() + 1); QCOMPARE(py(v[13], sizes.first.height()), expected.bottom() + 1);
            QCOMPARE(v[2], 0.0f); QCOMPARE(v[3], 0.0f); QCOMPARE(v[14], 1.0f); QCOMPARE(v[15], 1.0f);
        }
    }
    void damageUpdatesRejectIncompatibleGeometry() {
        TestDesktop view(false);
        auto original = solidFrame(Qt::red);
        DesktopUpdate first{original.size(), {1, 1, 100, 100, 0}, true, {{original.rect(), original}}};
        QVERIFY(view.applyUpdate(first));
        const auto accepted = view.frameRevision();
        QVERIFY(!view.applyUpdate(first));
        auto resized = solidFrame(Qt::blue, {original.width() + 64, original.height()});
        DesktopUpdate invalid{resized.size(), {1, 2, 200, 200, 0}, true, {{resized.rect(), resized}}};
        QVERIFY(!view.applyUpdate(invalid));
        QCOMPARE(view.frameRevision(), accepted); QCOMPARE(view.frame(), original);
        invalid.stamp.geometry = 2; invalid.full = false;
        QVERIFY(!view.applyUpdate(invalid));
        invalid.full = true; QVERIFY(view.applyUpdate(invalid)); QCOMPARE(view.frame(), resized);
        first.stamp.revision = 3; QVERIFY(!view.applyUpdate(first)); QCOMPARE(view.frame(), resized);
        view.setFrame({}); QVERIFY(view.applyUpdate(first)); QCOMPARE(view.frame(), original);
    }
    void partialUploadMatchesFullFrame() {
        REQUIRE_RHI();
        TestDesktop partial(false), full(false); partial.resize(512, 384); full.resize(512, 384);
        auto trace = std::make_shared<PerformanceTrace>(true); partial.setPerformanceTrace(trace);
        QImage source(1024, 768, QImage::Format_RGB32); source.fill(Qt::black);
        DesktopUpdateAccumulator pending;
        QVERIFY(pending.capture(source.constBits(), source.size(), source.bytesPerLine(), source.rect(), {1, 1, 100, 100, 0}).accepted);
        QVERIFY(partial.applyUpdate(pending.take())); full.setFrame(source); partial.show(); full.show();
        QSignalSpy partialSubmitted(&partial, &DesktopView::framePresented), fullSubmitted(&full, &DesktopView::framePresented);
        QTRY_VERIFY(revisionSubmitted(partialSubmitted, partial.frameRevision()));
        QTRY_VERIFY(revisionSubmitted(fullSubmitted, full.frameRevision()));
        const auto* retained = partial.frame().constBits();
        const QRect a(16, 16, 24, 24), b(904, 650, 24, 24);
        auto paint = [&](QRect rectangle, QRgb color) {
            for (int y = rectangle.top(); y <= rectangle.bottom(); ++y) {
                auto* row = reinterpret_cast<QRgb*>(source.scanLine(y));
                std::fill(row + rectangle.left(), row + rectangle.right() + 1, color);
            }
        };
        paint(a, qRgb(255, 0, 0));
        QVERIFY(pending.capture(source.constBits(), source.size(), source.bytesPerLine(), a, {1, 2, 200, 200, 0}).accepted);
        paint(b, qRgb(0, 255, 0));
        QVERIFY(pending.capture(source.constBits(), source.size(), source.bytesPerLine(), b, {1, 3, 300, 300, 0}).accepted);
        auto update = pending.take(); QVERIFY(!update.full); QVERIFY(partial.applyUpdate(update));
        QCOMPARE(partial.frame().constBits(), retained); // No unnecessary internal QImage detach.
        full.setFrame(source);
        QTRY_VERIFY(revisionSubmitted(partialSubmitted, partial.frameRevision()));
        QTRY_VERIFY(revisionSubmitted(fullSubmitted, full.frameRevision()));
        QCOMPARE(partial.frame(), source); QCOMPARE(partial.grabFramebuffer(), full.grabFramebuffer());
        quint64 latestUpload = 0;
        for (const auto& sample : trace->samples()) if (sample.stage == PerformanceStage::UploadPreparation) latestUpload = sample.bytes;
        QVERIFY(latestUpload > 0 && latestUpload < quint64(source.sizeInBytes()));
        const auto accepted = partial.frameRevision();
        update.stamp.geometry = 0; QVERIFY(!partial.applyUpdate(update)); QCOMPARE(partial.frameRevision(), accepted);
        partialSubmitted.clear(); partial.dropResources();
        QTRY_VERIFY(revisionSubmitted(partialSubmitted, accepted));
        QCOMPARE(partial.grabFramebuffer(), full.grabFramebuffer());
        for (const auto& sample : trace->samples()) if (sample.stage == PerformanceStage::UploadPreparation) latestUpload = sample.bytes;
        QCOMPARE(latestUpload, quint64(source.sizeInBytes())); QCOMPARE(trace->dropped(), quint64(0));
    }
    void partialUploadSurvivesResourceFailure() {
        REQUIRE_RHI();
        TestDesktop view(false); view.resize(512, 384); view.show();
        DesktopUpdateAccumulator pending;
        auto source = solidFrame(Qt::black, {1024, 768});
        QVERIFY(pending.capture(source.constBits(), source.size(), source.bytesPerLine(), source.rect(), {1, 1, 100, 100, 0}).accepted);
        QVERIFY(view.applyUpdate(pending.take()));
        QSignalSpy submitted(&view, &DesktopView::framePresented);
        QTRY_VERIFY(revisionSubmitted(submitted, view.frameRevision()));
        view.failStage = "texture"; view.dropResources(); QTRY_VERIFY(!view.rendererAvailable());
        const QRect rectangle(480, 352, 64, 64);
        for (int y = rectangle.top(); y <= rectangle.bottom(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(source.scanLine(y));
            std::fill(row + rectangle.left(), row + rectangle.right() + 1, qRgb(0, 255, 0));
        }
        QVERIFY(pending.capture(source.constBits(), source.size(), source.bytesPerLine(), rectangle, {1, 2, 200, 200, 0}).accepted);
        const auto batch = pending.take(); QVERIFY(!batch.full); QVERIFY(view.applyUpdate(batch));
        const auto accepted = view.frameRevision(); QTRY_VERIFY(!view.rendererAvailable());
        QCOMPARE(view.frame(), source);
        view.failStage.clear(); view.update();
        QTRY_VERIFY(revisionSubmitted(submitted, accepted)); QVERIFY(view.rendererAvailable());
        QCOMPARE(capturedCenter(view), QColor(Qt::green));
    }
    void presentationPacingStaysEventDriven() {
        REQUIRE_RHI();
        TestDesktop view(false); view.resize(512, 384); view.show();
        DesktopUpdateAccumulator pending;
        auto source = solidFrame(Qt::black, {1024, 768});
        QVERIFY(pending.capture(source.constBits(), source.size(), source.bytesPerLine(), source.rect(), {1, 1, 100, 100, 0}).accepted);
        QVERIFY(view.applyUpdate(pending.take()));
        QSignalSpy submitted(&view, &DesktopView::framePresented);
        QTRY_VERIFY(revisionSubmitted(submitted, view.frameRevision()));
        QTest::qWait(100); const auto idle = view.renders; QTest::qWait(350); QVERIFY(view.renders - idle <= 1);
    }
    void presentationBurstKeepsNewestRevision() {
        REQUIRE_RHI();
        TestDesktop view(false); view.resize(512, 384); view.show();
        auto trace = std::make_shared<PerformanceTrace>(true); view.setPerformanceTrace(trace);
        DesktopUpdateAccumulator pending;
        auto source = solidFrame(Qt::black, {1024, 768});
        QVERIFY(pending.capture(source.constBits(), source.size(), source.bytesPerLine(), source.rect(), {1, 1, 100, 100, 0}).accepted);
        QVERIFY(view.applyUpdate(pending.take()));
        QSignalSpy submitted(&view, &DesktopView::framePresented);
        QTRY_VERIFY(revisionSubmitted(submitted, view.frameRevision()));
        QTest::qWait(100); submitted.clear(); const auto before = view.renders;
        const QRect rectangle(480, 352, 64, 64);
        for (quint64 revision = 2; revision <= 201; ++revision) {
            for (int y = rectangle.top(); y <= rectangle.bottom(); ++y) {
                auto* row = reinterpret_cast<QRgb*>(source.scanLine(y));
                std::fill(row + rectangle.left(), row + rectangle.right() + 1, qRgb(int(revision), 30, 240 - int(revision)));
            }
            const auto now = PerformanceTrace::now();
            QVERIFY(pending.capture(source.constBits(), source.size(), source.bytesPerLine(), rectangle, {1, revision, now, now, 0}).accepted);
            QVERIFY(view.applyUpdate(pending.take()));
        }
        QTRY_VERIFY(revisionSubmitted(submitted, view.frameRevision()));
        QVERIFY(view.renders - before <= 2); QVERIFY(submitted.size() <= 2);
        QCOMPARE(view.frame(), source); QCOMPARE(capturedCenter(view), QColor(201, 30, 39));
        bool newest = false;
        for (const auto& sample : trace->samples()) if (sample.stage == PerformanceStage::WindowSubmission && sample.frame.revision == 201) {
            newest = true; QVERIFY(sample.frame.replacements >= 199);
        }
        QVERIFY(newest); QCOMPARE(trace->dropped(), quint64(0));
    }
    void screenshotCannotCountAsPresentation() {
        performanceGrabCannotRecordWindowSubmission();
    }
    void rhiPresentsFrameInsideShownMainWindow() {
        REQUIRE_RHI();
        QTemporaryDir dir; MainWindow window(dir.filePath("confCons.xml"), true);
        window.resize(1000, 750); window.show(); WAIT_STORE(window); QTest::qWait(100);
        window.quickConnect("renderer.invalid", true);
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs");
        auto* pane = qobject_cast<SessionPane*>(tabs->currentWidget()); QVERIFY(pane);
        auto* view = pane->findChild<DesktopView*>(); QVERIFY(view);
        QSignalSpy submitted(view, &DesktopView::framePresented), failed(view, &DesktopView::renderFailed);
        QImage frame(200, 100, QImage::Format_RGB32); frame.fill(Qt::darkCyan); view->setFrame(frame);
        const auto revision = view->frameRevision();
        QTRY_VERIFY_WITH_TIMEOUT(revisionSubmitted(submitted, revision), 10000); QVERIFY(failed.isEmpty());
        const auto shot = view->grabFramebuffer(); QVERIFY(!shot.isNull());
        QCOMPARE(shot.pixelColor(shot.width() / 2, shot.height() / 2), QColor(Qt::darkCyan));
        window.closeTab(tabs->currentIndex());
    }
    void rhiRetainsFrameAfterResourceRelease() {
        REQUIRE_RHI();
        QWidget first, second; auto* one = new QVBoxLayout(&first); auto* two = new QVBoxLayout(&second);
        first.resize(420, 320); second.resize(560, 400); first.show(); second.show();
        auto* view = new TestDesktop(false); one->addWidget(view); view->show();
        QSignalSpy submitted(view, &DesktopView::framePresented), failed(view, &DesktopView::renderFailed);
        view->setFrame(solidFrame(Qt::magenta)); const auto revision = view->frameRevision();
        QTRY_VERIFY(revisionSubmitted(submitted, revision)); QCOMPARE(capturedCenter(*view), QColor(Qt::magenta));
        submitted.clear(); view->dropResources();
        QTRY_VERIFY(revisionSubmitted(submitted, revision)); QCOMPARE(capturedCenter(*view), QColor(Qt::magenta));
        submitted.clear(); two->addWidget(view); view->show(); view->update();
        QTRY_VERIFY(revisionSubmitted(submitted, revision)); QCOMPARE(view->frameRevision(), revision);
        QCOMPARE(capturedCenter(*view), QColor(Qt::magenta)); QVERIFY(failed.isEmpty());
        delete view;
    }
    void rhiNullFrameClearsPresentation() {
        REQUIRE_RHI(); TestDesktop view(false); view.resize(400, 300); view.show();
        QSignalSpy submitted(&view, &DesktopView::framePresented), failed(&view, &DesktopView::renderFailed);
        view.setFrame(solidFrame(Qt::red)); QTRY_VERIFY(revisionSubmitted(submitted, view.frameRevision()));
        view.setFrame({}); const auto cleared = view.frameRevision();
        QTRY_VERIFY(revisionSubmitted(submitted, cleared));
        QVERIFY(view.findChild<QLabel*>("desktopPlaceholder")->isVisible());
        QCOMPARE(capturedCenter(view), view.palette().color(QPalette::Base));
        view.setFrame(solidFrame(Qt::blue)); QTRY_VERIFY(revisionSubmitted(submitted, view.frameRevision()));
        QCOMPARE(capturedCenter(view), QColor(Qt::blue)); QVERIFY(failed.isEmpty());
    }
    void rhiTargetChangesPreserveFrame() {
        REQUIRE_RHI(); TestDesktop view(false); view.resize(400, 300); view.show();
        QSignalSpy submitted(&view, &DesktopView::framePresented), failed(&view, &DesktopView::renderFailed);
        QCOMPARE(view.sampleCount(), 1); view.setFrame(solidFrame(Qt::yellow));
        const auto revision = view.frameRevision(); QTRY_VERIFY(revisionSubmitted(submitted, revision));
        QVERIFY(view.supportedSamples.contains(4));
        for (const auto format : {QRhiWidget::TextureFormat::RGBA8, QRhiWidget::TextureFormat::RGBA16F}) {
            for (const int samples : {4, 1}) {
                submitted.clear(); view.setColorBufferFormat(format); view.setSampleCount(samples);
                view.resize(view.width() + 20, view.height() + 10); view.update();
                QTRY_VERIFY(revisionSubmitted(submitted, revision)); QCOMPARE(view.targetSamples, samples);
                if (samples > 1) QVERIFY(view.msaaWithoutColorTexture);
                QCOMPARE(capturedCenter(view), QColor(Qt::yellow)); QCOMPARE(view.frameRevision(), revision);
                QVERIFY(failed.isEmpty());
            }
        }
    }
    void rhiRetriesFailedResourceCreation_data() {
        QTest::addColumn<QByteArray>("stage");
        for (const char* stage : {"shaders", "sampler", "buffer", "texture", "bindings", "pipeline"}) QTest::newRow(stage) << QByteArray(stage);
    }
    void rhiRetriesFailedResourceCreation() {
        REQUIRE_RHI(); QFETCH(QByteArray, stage);
        TestDesktop view(false); view.failStage = stage; view.resize(400, 300);
        QSignalSpy submitted(&view, &DesktopView::framePresented);
        view.setFrame(solidFrame(Qt::darkGreen)); const auto revision = view.frameRevision(); view.show();
        QTRY_VERIFY(!view.rendererAvailable()); QVERIFY(!revisionSubmitted(submitted, revision));
        auto* placeholder = view.findChild<QLabel*>("desktopPlaceholder");
        QVERIFY(placeholder->isVisible()); QVERIFY(placeholder->text().contains("Renderer unavailable"));
        QCOMPARE(view.frame().pixelColor(0, 0), QColor(Qt::darkGreen));
        QTest::qWait(100); const int idle = view.renders; QTest::qWait(250); QVERIFY(view.renders - idle <= 1);
        view.failStage.clear(); view.update(); // No replacement frame: retained upload must retry.
        QTRY_VERIFY(revisionSubmitted(submitted, revision)); QVERIFY(view.rendererAvailable()); QVERIFY(placeholder->isHidden());
        QCOMPARE(capturedCenter(view), QColor(Qt::darkGreen));
        // Repeat a failed cache rebuild, then retry with a same-dimension frame.
        view.failStage = stage; view.dropResources(); QTRY_VERIFY(!view.rendererAvailable());
        view.failStage.clear(); view.setFrame(solidFrame(Qt::darkBlue));
        QTRY_VERIFY(revisionSubmitted(submitted, view.frameRevision())); QCOMPARE(capturedCenter(view), QColor(Qt::darkBlue));
    }
    void rhiUploadOwnsPixelsAndCoalesces() {
        REQUIRE_RHI(); TestDesktop view(false); view.resize(404, 300); view.show();
        QSignalSpy submitted(&view, &DesktopView::framePresented), failed(&view, &DesktopView::renderFailed);
        auto source = solidFrame(Qt::darkMagenta, {101, 57}); view.setFrame(source);
        source.fill(Qt::black); source = {};
        QTRY_VERIFY(revisionSubmitted(submitted, view.frameRevision())); QCOMPARE(capturedCenter(view), QColor(Qt::darkMagenta));
        constexpr int stride = 512;
        auto* data = new uchar[stride * 57];
        QImage padded(data, 101, 57, stride, QImage::Format_RGB32, [](void* pixels) { delete[] static_cast<uchar*>(pixels); }, data);
        padded.fill(QColor(17, 91, 203)); QCOMPARE(padded.bytesPerLine(), stride);
        view.setFrame(std::move(padded)); const auto first = view.frameRevision();
        QTRY_VERIFY(revisionSubmitted(submitted, first)); QCOMPARE(capturedCenter(view), QColor(17, 91, 203));
        // Mutate/release the source after setFrame, replace before the previous submission.
        view.afterRender = [&view] { view.setFrame(solidFrame(QColor(201, 42, 9), {101, 57})); };
        view.setFrame(solidFrame(QColor(5, 67, 231), {101, 57})); const auto beforeReplace = view.frameRevision();
        QTRY_VERIFY(revisionSubmitted(submitted, beforeReplace));
        QTRY_VERIFY(revisionSubmitted(submitted, beforeReplace + 1)); QCOMPARE(capturedCenter(view), QColor(201, 42, 9));
        QTest::qWait(100); submitted.clear(); const auto count = view.renders;
        for (int i = 0; i < 200; ++i) view.setFrame(solidFrame(QColor(i, 30, 240 - i), {101, 57}));
        const auto latest = view.frameRevision(); QTRY_VERIFY(revisionSubmitted(submitted, latest));
        QVERIFY(view.renders - count <= 2); QVERIFY(submitted.size() <= 2);
        QCOMPARE(capturedCenter(view), QColor(199, 30, 41)); QVERIFY(failed.isEmpty());
        QTest::qWait(100); const auto idle = view.renders; QTest::qWait(350); QVERIFY(view.renders - idle <= 1);
        // A grab-only render and an unrelated composition are not presentation evidence.
        view.setUpdatesEnabled(false); submitted.clear(); view.setFrame(solidFrame(Qt::cyan));
        const auto unpainted = view.frameRevision(); QVERIFY(!view.grabFramebuffer().isNull());
        QMetaObject::invokeMethod(&view, "frameSubmitted", Qt::DirectConnection);
        QVERIFY(!revisionSubmitted(submitted, unpainted));
        view.setUpdatesEnabled(true); view.update(); QTRY_VERIFY(revisionSubmitted(submitted, unpainted));
    }
    void rhiMultipleSessionsRemainIndependent() {
        REQUIRE_RHI();
        QTemporaryDir dir; MainWindow window(dir.filePath("confCons.xml"), true);
        window.resize(1000, 750); window.show(); WAIT_STORE(window);
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs");
        window.quickConnect("one.invalid", true); auto* first = qobject_cast<SessionPane*>(tabs->currentWidget()); QVERIFY(first);
        auto* a = first->findChild<DesktopView*>(); QVERIFY(a); a->setFrame(solidFrame(Qt::red));
        QSignalSpy shownA(a, &DesktopView::framePresented); QTRY_VERIFY(revisionSubmitted(shownA, a->frameRevision()));
        window.quickConnect("two.invalid", true); auto* second = qobject_cast<SessionPane*>(tabs->currentWidget()); QVERIFY(second);
        auto* b = second->findChild<DesktopView*>(); QVERIFY(b); QSignalSpy shownB(b, &DesktopView::framePresented);
        b->setFrame(solidFrame(Qt::blue)); QTRY_VERIFY(revisionSubmitted(shownB, b->frameRevision()));
        for (int i = 0; i < 12; ++i) {
            a->setFrame(solidFrame(i % 2 ? Qt::red : Qt::yellow)); b->setFrame(solidFrame(i % 2 ? Qt::blue : Qt::cyan));
            auto* active = i % 2 ? a : b; auto& spy = i % 2 ? shownA : shownB;
            tabs->setCurrentWidget(i % 2 ? first : second); active->update();
            QTRY_VERIFY(revisionSubmitted(spy, active->frameRevision()));
            QCOMPARE(capturedCenter(*active), QColor(i % 2 ? Qt::red : Qt::cyan));
        }
        // Move an entire session subtree to a second, already shown top-level host.
        QWidget other; auto* layout = new QVBoxLayout(&other); other.resize(500, 400); other.show();
        tabs->removeTab(tabs->indexOf(second)); shownB.clear(); layout->addWidget(second); second->show(); b->update();
        QTRY_VERIFY(revisionSubmitted(shownB, b->frameRevision())); QCOMPARE(capturedCenter(*b), QColor(Qt::blue));
        tabs->setCurrentWidget(first); a->update(); QTRY_VERIFY(revisionSubmitted(shownA, a->frameRevision()));
        QCOMPARE(capturedCenter(*a), QColor(Qt::red));
        tabs->addTab(second, "Moved back"); tabs->setCurrentWidget(second);
        QPointer<SessionPane> dead = second; window.closeTab(tabs->indexOf(second)); QVERIFY(!dead);
        window.closeTab(tabs->indexOf(first));
        for (int i = 0; i < 8; ++i) {
            window.quickConnect("cycle.invalid", true); auto* view = tabs->currentWidget()->findChild<DesktopView*>();
            QSignalSpy shown(view, &DesktopView::framePresented); view->setFrame(solidFrame(Qt::green));
            QTRY_VERIFY(revisionSubmitted(shown, view->frameRevision())); window.disconnectCurrent();
            window.reconnectCurrent(); view = tabs->currentWidget()->findChild<DesktopView*>(); QVERIFY(view->frame().isNull());
            QSignalSpy fresh(view, &DesktopView::framePresented); view->setFrame(solidFrame(Qt::magenta));
            QTRY_VERIFY(revisionSubmitted(fresh, view->frameRevision())); window.closeTab(tabs->currentIndex());
        }
    }
    void rhiRepeatedShutdownReleasesResources() {
        REQUIRE_RHI();
        // Qt 6.12 release builds expose their native-resource leak check through this flag.
        QCOMPARE(qEnvironmentVariableIntValue("QT_RHI_LEAK_CHECK"), 1);
        for (int i = 0; i < 12; ++i) {
            auto view = std::make_unique<TestDesktop>(false); view->resize(400, 300); view->show();
            QSignalSpy submitted(view.get(), &DesktopView::framePresented);
            view->setFrame(solidFrame(Qt::green)); QTRY_VERIFY(revisionSubmitted(submitted, view->frameRevision()));
            view->setFrame(solidFrame(Qt::red, {300, 200})); QTRY_VERIFY(revisionSubmitted(submitted, view->frameRevision()));
            view.reset(); // Both DesktopView resources and this top-level QRhi must be released.
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }
    void rhiHighDpiPreservesGeometryAndInput() {
        REQUIRE_RHI(); TestDesktop view(false); view.resize(400, 300); view.show();
        QSignalSpy submitted(&view, &DesktopView::framePresented), pointers(&view, &DesktopView::pointerInput);
        view.setFrame(solidFrame(Qt::darkCyan, {1280, 720})); const auto revision = view.frameRevision();
        QTRY_VERIFY(revisionSubmitted(submitted, revision));
        QSet<qreal> ratios;
        for (auto* screen : QGuiApplication::screens()) {
            view.windowHandle()->setScreen(screen); view.move(screen->availableGeometry().center() - view.rect().center());
            QTest::qWait(100); submitted.clear(); view.update(); QTRY_VERIFY(revisionSubmitted(submitted, revision));
            ratios.insert(view.devicePixelRatioF()); const auto shot = view.grabFramebuffer(); QVERIFY(!shot.isNull());
            QCOMPARE(shot.size(), QSize(qRound(view.width() * view.devicePixelRatioF()), qRound(view.height() * view.devicePixelRatioF())));
            QSize scaled = view.frame().size().scaled(view.size(), Qt::KeepAspectRatio);
            const QRect area(QPoint((view.width() - scaled.width()) / 2, (view.height() - scaled.height()) / 2), scaled);
            const qreal sx = qreal(shot.width()) / view.width(), sy = qreal(shot.height()) / view.height();
            QCOMPARE(shot.pixelColor(int(area.center().x() * sx), int(area.top() / 2 * sy)), QColor(Qt::black));
            QCOMPARE(capturedCenter(view), QColor(Qt::darkCyan)); pointers.clear();
            const QPoint point = area.topLeft() + QPoint(area.width() / 4, area.height() * 3 / 4);
            QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, point); QVERIFY(pointers.size() >= 2);
            QCOMPARE(pointers.at(0).at(1).toInt(), (point.x() - area.x()) * 1280 / area.width());
            QCOMPARE(pointers.at(0).at(2).toInt(), (point.y() - area.y()) * 720 / area.height());
            QCOMPARE(view.frameRevision(), revision);
        }
        if (ratios.size() < 2) QSKIP("Current display DPR verified; no different-DPR display for a live transition. Run at alternate QT_SCALE_FACTOR too.");
    }
    void clipboardDefaultsOffAndNeverTouchesHost() {
        auto core = rm::detached_core();
        auto host = std::make_shared<TestClipboardEndpoint>();
        SessionPane pane(core->open_temporary_session(bridgeTarget(SessionTarget{"clipboard.invalid"}), true), {}, nullptr, {}, host);
        QVERIFY2(pane.metaObject()->indexOfProperty("clipboardSharingEnabled") >= 0, "Session clipboard permission must be available without a credential prompt");
        QVERIFY(!pane.property("clipboardSharingEnabled").toBool());
        pane.setClipboardSharingEnabled(true); QVERIFY(!pane.clipboardSharingEnabled());
        host->copy("synthetic host copy"); pane.shutdown();
        QCOMPARE(host->reads, 0); QCOMPARE(host->writes, 0); QCOMPARE(host->watches, 0);
    }
    void remoteCursorScalesWithDesktopAndDpr() {
        QImage image(16, 20, QImage::Format_ARGB32); image.fill(Qt::red);
        RemoteCursor cursor{RemoteCursor::Kind::Bitmap, image, {5, 7}};
        for (qreal dpr : {1.0, 1.5, 2.0}) {
            const auto scaled = scaleRemoteCursor(cursor, {200, 100}, {400, 200}, dpr);
            QCOMPARE(scaled.image.size(), QSize(qRound(32 * dpr), qRound(40 * dpr)));
            QCOMPARE(scaled.image.devicePixelRatio(), dpr); QCOMPARE(scaled.hotspot, QPoint(10, 14));
        }
        DesktopView view(false); view.resize(400, 300); view.setFrame(solidFrame(Qt::blue));
        QMouseEvent move(QEvent::MouseMove, QPointF(200, 150), QPointF(200, 150), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&view, &move); view.setRemoteCursor(cursor);
        QCOMPARE(view.cursor().shape(), Qt::BitmapCursor); QCOMPARE(view.cursor().hotSpot(), QPoint(10, 14));
        const auto pixmap = view.cursor().pixmap();
        QCOMPARE(pixmap.size(), QSize(qRound(32 * view.devicePixelRatioF()), qRound(40 * view.devicePixelRatioF())));
        view.setFrame(solidFrame(Qt::blue, {400, 200})); QCOMPARE(view.cursor().hotSpot(), QPoint(5, 7));
        view.resetRemoteCursor(); QCOMPARE(view.cursor().shape(), Qt::ArrowCursor);
    }
    void remoteCursorRestoresOverLetterbox() {
        DesktopView view(false); view.resize(400, 300); view.setFrame(solidFrame(Qt::blue));
        const auto move = [&view](QPoint position) {
            QMouseEvent event(QEvent::MouseMove, QPointF(position), QPointF(position), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&view, &event);
        };
        move({200, 150}); view.setRemoteCursor({RemoteCursor::Kind::Hidden}); QCOMPARE(view.cursor().shape(), Qt::BlankCursor);
        move({200, 10}); QCOMPARE(view.cursor().shape(), Qt::ArrowCursor);
        move({200, 150}); QCOMPARE(view.cursor().shape(), Qt::BlankCursor);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(200, 150), QPointF(200, 150), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&view, &press); move({200, 10}); QCOMPARE(view.cursor().shape(), Qt::BlankCursor);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(200, 10), QPointF(200, 10), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&view, &release); QCOMPARE(view.cursor().shape(), Qt::ArrowCursor);
        move({200, 150}); QEvent leave(QEvent::Leave); QApplication::sendEvent(&view, &leave); QCOMPARE(view.cursor().shape(), Qt::ArrowCursor);
        view.setFrame({}); QCOMPARE(view.cursor().shape(), Qt::ArrowCursor);
    }
#ifdef RM_WITH_RDP
    void remoteCursorRejectsRetiredGeneration() {
        QTemporaryDir dir; auto core = rm::detached_core();
        SessionPane pane(core->open_temporary_session(bridgeTarget(SessionTarget{"127.0.0.1", 1}), true), {});
        auto* view = pane.findChild<DesktopView*>(); QVERIFY(view);
        RdpConfig config; config.host = "127.0.0.1"; config.port = 1; config.configDirectory = dir.path();
        pane.startRdp(config); auto* old = pane.findChild<RdpWorker*>(); QVERIFY(old);
        QVERIFY(old->metaObject()->indexOfSignal("cursorAvailable()") >= 0);
        view->setFrame(solidFrame(Qt::red));
        QMouseEvent move(QEvent::MouseMove, QPointF(100, 75), QPointF(100, 75), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view, &move); view->setRemoteCursor({RemoteCursor::Kind::Hidden});
        QCOMPARE(view->cursor().shape(), Qt::BlankCursor);
        QMetaObject::invokeMethod(old, "cursorAvailable", Qt::DirectConnection); pane.shutdown();
        QCOMPARE(view->cursor().shape(), Qt::ArrowCursor); QTest::qWait(50); QCOMPARE(view->cursor().shape(), Qt::ArrowCursor);
        pane.startRdp(config); QCOMPARE(view->cursor().shape(), Qt::ArrowCursor); QVERIFY(view->frame().isNull()); pane.shutdown();
    }
    void sessionFramesRejectDisconnectedGenerations() {
        QTemporaryDir dir; MainWindow window(dir.filePath("confCons.xml"), true); window.show(); WAIT_STORE(window);
        window.quickConnect("generation.invalid", true);
        auto* pane = qobject_cast<SessionPane*>(window.findChild<QTabWidget*>("sessionTabs")->currentWidget()); QVERIFY(pane);
        auto* view = pane->findChild<DesktopView*>(); QVERIFY(view);
        auto config = [&] { RdpConfig c; c.host = "127.0.0.1"; c.port = 1; c.configDirectory = dir.path(); return c; };
        pane->startRdp(config()); auto* old = pane->findChild<RdpWorker*>(); QVERIFY(old);
        view->setFrame(solidFrame(Qt::red)); QMetaObject::invokeMethod(old, "frameAvailable", Qt::DirectConnection);
        pane->shutdown(); const auto disconnected = view->frameRevision(); QTest::qWait(50);
        QCOMPARE(view->frameRevision(), disconnected); QCOMPARE(view->frame().pixelColor(0, 0), QColor(Qt::red));
        pane->startRdp(config()); old = pane->findChild<RdpWorker*>(); QVERIFY(old);
        view->setFrame(solidFrame(Qt::blue)); QMetaObject::invokeMethod(old, "frameAvailable", Qt::DirectConnection);
        pane->startRdp(config()); const auto fresh = view->frameRevision(); QVERIFY(view->frame().isNull());
        QTest::qWait(50); QCOMPARE(view->frameRevision(), fresh); QVERIFY(view->frame().isNull());
        pane->shutdown(); window.close();
    }
#endif
    void rhiPresentsFrameAndGrabMatches() {
        for (const char* path : {":/rm/shaders/desktop.vert.qsb", ":/rm/shaders/desktop.frag.qsb"}) {
            QFile shader(QString::fromLatin1(path)); QVERIFY(shader.open(QIODevice::ReadOnly));
            QVERIFY(QShader::fromSerialized(shader.readAll()).isValid());
        }
        DesktopView view(false); view.resize(400, 300); view.show();
        auto* placeholder = view.findChild<QLabel*>("desktopPlaceholder"); QVERIFY(placeholder && placeholder->isVisible());
        QImage frame(200, 100, QImage::Format_RGB32); frame.fill(Qt::red);
        QPainter painter(&frame);
        painter.fillRect(100, 0, 100, 100, Qt::blue); painter.fillRect(0, 50, 100, 50, Qt::green); painter.fillRect(100, 50, 100, 50, Qt::yellow);
        painter.end();
        QSignalSpy submitted(&view, &DesktopView::framePresented), failed(&view, &DesktopView::renderFailed);
        view.setFrame(frame); QCOMPARE(view.frame().size(), QSize(200, 100));
        if (QGuiApplication::platformName() == "offscreen") {
            QVERIFY(placeholder->isVisible()); QVERIFY(placeholder->text().contains("Renderer unavailable"));
            QSKIP("Offscreen has no RHI; unavailable UX verified, presentation requires a real backend");
        }
        QVERIFY(placeholder->isHidden());
        QTRY_VERIFY_WITH_TIMEOUT(revisionSubmitted(submitted, view.frameRevision()), 10000); QVERIFY(failed.isEmpty());
        const QImage shot = view.grabFramebuffer(); QVERIFY(!shot.isNull());
        const qreal sx = qreal(shot.width()) / view.width(), sy = qreal(shot.height()) / view.height();
        QSize scaled = frame.size(); scaled.scale(view.size(), Qt::KeepAspectRatio);
        const QRect area(QPoint((view.width() - scaled.width()) / 2, (view.height() - scaled.height()) / 2), scaled);
        const auto at = [&](int x, int y) { return shot.pixelColor(int(x * sx), int(y * sy)); };
        QCOMPARE(at(area.left() + area.width() / 4, area.top() + area.height() / 4), QColor(Qt::red));
        QCOMPARE(at(area.left() + area.width() * 3 / 4, area.top() + area.height() / 4), QColor(Qt::blue));
        QCOMPARE(at(area.left() + area.width() / 4, area.top() + area.height() * 3 / 4), QColor(Qt::green));
        QCOMPARE(at(area.left() + area.width() * 3 / 4, area.top() + area.height() * 3 / 4), QColor(Qt::yellow));
        QVERIFY(area.top() > 0); QCOMPARE(at(area.center().x(), area.top() / 2), QColor(Qt::black));
        QImage second(300, 200, QImage::Format_RGB32); second.fill(Qt::darkCyan); view.setFrame(second);
        QTRY_VERIFY_WITH_TIMEOUT(revisionSubmitted(submitted, view.frameRevision()), 10000); QVERIFY(failed.isEmpty());
        const QImage shot2 = view.grabFramebuffer(); QVERIFY(!shot2.isNull());
        const qreal sx2 = qreal(shot2.width()) / view.width(), sy2 = qreal(shot2.height()) / view.height();
        QSize scaled2 = second.size(); scaled2.scale(view.size(), Qt::KeepAspectRatio);
        const QRect area2(QPoint((view.width() - scaled2.width()) / 2, (view.height() - scaled2.height()) / 2), scaled2);
        QCOMPARE(shot2.pixelColor(int(area2.center().x() * sx2), int(area2.center().y() * sy2)), QColor(Qt::darkCyan));
    }
    void scanCodesAndFocusRelease() {
        QCOMPARE(DesktopView::scanCode(Qt::Key_A),quint32(0x1e));
        QCOMPARE(DesktopView::scanCode(Qt::Key_Left),quint32(0x14b));
        DesktopView view(false); QSignalSpy keys(&view,&DesktopView::keyInput); view.show();
        QTest::keyClick(&view,Qt::Key_Tab); QVERIFY(!keys.empty());
        QCOMPARE(keys.last().at(0).value<quint32>(),quint32(0x0f));
        QTest::keyPress(&view,Qt::Key_Control); QVERIFY(!keys.empty());
        QFocusEvent out(QEvent::FocusOut); QCoreApplication::sendEvent(&view,&out);
        QCOMPARE(keys.last().at(1).toBool(),false);
        QImage frame(100,100,QImage::Format_RGB32); frame.fill(Qt::black); view.setFrame(frame);
        QSignalSpy pointer(&view,&DesktopView::pointerInput);
        QTest::mousePress(&view,Qt::LeftButton,Qt::NoModifier,view.rect().center());
        QVERIFY(!pointer.empty());
        QTest::mouseRelease(&view,Qt::LeftButton,Qt::NoModifier,QPoint(-10,-10));
        QCOMPARE(pointer.last().at(0).value<quint16>(),quint16(0x1000));
        QTest::mousePress(&view,Qt::RightButton,Qt::NoModifier,view.rect().center());
        QCoreApplication::sendEvent(&view,&out);
        QCOMPARE(pointer.last().at(0).value<quint16>(),quint16(0x2000));
    }
#ifdef RM_WITH_RDP
    void framebufferCopiesSurviveResizeAndFree() {
        QByteArray bytes(4 * 4 * 4,char(0x7f));
        auto frame = RdpWorker::copyFramebuffer(reinterpret_cast<const uchar*>(bytes.constData()),4,4,16);
        QVERIFY(!frame.isNull()); const QColor before = frame.pixelColor(0,0);
        bytes.fill(0); bytes.clear(); bytes.squeeze();
        QCOMPARE(frame.pixelColor(0,0),before);
        QVERIFY(RdpWorker::copyFramebuffer(nullptr,4,4,16).isNull());
        QVERIFY(RdpWorker::copyFramebuffer(reinterpret_cast<const uchar*>("x"),8193,1,32772).isNull());
    }
    void cancelConnectDuringResizeAndDestroy() {
        for (int i=0; i<3; ++i) {
            RdpConfig c; c.host = "127.0.0.1"; c.port = 1; c.configDirectory = qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/freerdp";
            auto worker = std::make_unique<RdpWorker>(std::move(c));
            worker->start(); worker->resizeDesktop(QSize(800,600));
            QTest::qWait(20); worker->stop(); QVERIFY(worker->wait(10000));
        }
    }
#endif
};
QTEST_MAIN(Smoke)
#include "qt_smoke.moc"
