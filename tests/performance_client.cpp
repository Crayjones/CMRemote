#include "RdpWorker.h"
#include "Views.h"
#include "RhiPixelOracle.h"
#include "PerformanceMarker.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtMath>
#include <QVBoxLayout>
#include <QWindow>

#ifdef Q_OS_MACOS
void activateNativeTestApplication(QWidget* widget);
#endif

class PerformanceClient : public QObject {
    Q_OBJECT
private slots:
    void ownedWorkload() {
        const int port = qEnvironmentVariableIntValue("RM_RDP_TEST_PORT");
        const int workload = qEnvironmentVariableIntValue("RM_PERFORMANCE_WORKLOAD");
        auto password = qgetenv("RM_RDP_TEST_PASSWORD");
        QVERIFY(port > 0 && port <= 65535 && !password.isEmpty());
        QVERIFY(workload >= 0 && workload < 6);
        QTemporaryDir directory(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/performance-XXXXXX");
        QVERIFY(directory.isValid());
        auto trace = std::make_shared<PerformanceTrace>(true, 1);
        RdpConfig config; config.host = "127.0.0.1"; config.port = quint16(port);
        config.username = "rdpfixture"; config.password = password; config.configDirectory = directory.path();
        config.options.automaticResize = false; config.performance = trace;
        const auto policy = qEnvironmentVariable("RM_PERFORMANCE_POLICY", "legacy");
        QVERIFY(policy == "legacy" || policy == "gfx");
        config.graphicsPolicy = policy == "gfx" ? RdpGraphicsPolicy::Gfx : RdpGraphicsPolicy::Legacy;
        password.fill(0); password.clear();
        QWidget window; QVBoxLayout layout(&window); layout.setContentsMargins(0, 0, 0, 0);
        DesktopView view(false); view.setPerformanceTrace(trace); layout.addWidget(&view);
        // Canonical baseline: one remote pixel per output pixel, matching SDL's default sizing.
        window.resize(qCeil(1024 / window.devicePixelRatioF()), qCeil(768 / window.devicePixelRatioF())); window.show();
        const bool native = QGuiApplication::platformName() != "offscreen";
#ifdef Q_OS_MACOS
        if (native) activateNativeTestApplication(&window);
#endif
        if (native) {
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            window.raise(); window.activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(&window));
        }
        RdpWorker worker(std::move(config));
        connect(&view, &DesktopView::keyInput, &worker, &RdpWorker::key);
        connect(&view, &DesktopView::textInput, &worker, &RdpWorker::text);
        connect(&view, &DesktopView::pointerInput, &worker, &RdpWorker::pointer);
        bool failed = false;
        connect(&worker, &RdpWorker::stateChanged, &window, [&](const QString& state, const QString&) { failed |= state == "failed"; });
        connect(&worker, &RdpWorker::frameAvailable, &window, [&] {
            const auto update = worker.takeUpdate();
            if (!update.patches.empty()) failed |= !view.applyUpdate(update);
        }, Qt::QueuedConnection);
        QSignalSpy submitted(&view, &DesktopView::framePresented), renderFailed(&view, &DesktopView::renderFailed);
        worker.start();
        QTRY_VERIFY_WITH_TIMEOUT(failed || view.frame().size() == QSize(1024, 768), 15000);
        QVERIFY(!failed);
        const auto start = PerformanceTrace::now();
        auto complete = [&] {
            const auto frame = view.frame();
            return !frame.isNull() && performanceMarker(frame.constBits(), frame.width(), frame.height(), frame.bytesPerLine()) == 436;
        };
        QElapsedTimer elapsed; elapsed.start();
        int inputs = 0;
        bool presentationLost = false;
        view.setFocus();
        while (!failed && !complete() && elapsed.elapsed() < 15000) {
            if (native && (!window.windowHandle()->isExposed() || !window.isActiveWindow())) {
                presentationLost = true; break;
            }
            if (workload == 5 && elapsed.elapsed() >= inputs * 100) {
                QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(96, 80));
                QTest::keyClick(&view, Qt::Key_A); ++inputs;
            }
            QTest::qWait(5);
        }
        QVERIFY2(!presentationLost, "Native performance window lost exposure/activation during motion; capture invalid");
        QVERIFY(!failed); QVERIFY(complete());
        if (native) {
            qInfo("RM_APP_WINDOW_STATE=%d,%d,%d,%d,%d,%d", int(window.isVisible()),
                    int(window.windowHandle()->isExposed()), int(window.isActiveWindow()),
                    int(view.rendererAvailable()), int(submitted.count()), int(renderFailed.count()));
            QVERIFY2(window.windowHandle()->isExposed() && window.isActiveWindow(),
                "Native performance window lost exposure/activation; capture invalid");
            QTRY_COMPARE(view.presentedRevision(), view.frameRevision());
            QVERIFY(renderFailed.isEmpty()); QVERIFY(view.rendererAvailable());
        }
        QTest::qWait(150);
        const auto end = PerformanceTrace::now();
        worker.stop(); QVERIFY(worker.wait(15000));
        const auto frame = view.frame();
        QJsonArray pixels;
        for (int y = 32; y < 768; y += 32) for (int x = 16; x < 1024; x += 32)
            pixels.append(int(frame.pixel(x, y) & 0xffffff));
        QSize pixelSize(qRound(view.width() * view.devicePixelRatioF()), qRound(view.height() * view.devicePixelRatioF()));
        if (native) {
            // One post-measurement grab checks owned pixels; it contributes no timing samples.
            const auto shot = view.grabFramebuffer(); QVERIFY(!shot.isNull()); pixelSize = shot.size();
            const auto scaled = frame.size().scaled(view.size(), Qt::KeepAspectRatio);
            const QRect area(QPoint((view.width() - scaled.width()) / 2, (view.height() - scaled.height()) / 2), scaled);
            for (int y = 32; y < 768; y += 32) for (int x = 16; x < 1024; x += 32) {
                const int px = int((area.left() + x * area.width() / 1024.0) * shot.width() / view.width());
                const int py = int((area.top() + y * area.height() / 768.0) * shot.height() / view.height());
                // Readback samples physical pixel centers, not remote texel centers.
                const double sx = ((px + 0.5) * view.width() / shot.width() - area.left()) * frame.width() / area.width() - 0.5;
                const double sy = ((py + 0.5) * view.height() / shot.height() - area.top()) * frame.height() / area.height() - 0.5;
                const QColor expected = linearPixel(frame, sx, sy), actual = shot.pixelColor(px, py);
                QVERIFY(qAbs(actual.red() - expected.red()) <= 1);
                QVERIFY(qAbs(actual.green() - expected.green()) <= 1);
                QVERIFY(qAbs(actual.blue() - expected.blue()) <= 1);
            }
        }
        QJsonObject report{{"schema", 1}, {"workload", workload}, {"width", frame.width()},
            {"height", frame.height()}, {"dpr", view.devicePixelRatioF()}, {"pixels", pixels},
            {"window_width", view.width()}, {"window_height", view.height()},
            {"pixel_width", pixelSize.width()}, {"pixel_height", pixelSize.height()},
            {"trace", QJsonDocument::fromJson(trace->summaryJson(start, end)).object()}};
        qInfo().noquote() << "RM_PERFORMANCE_RESULT=" + QJsonDocument(report).toJson(QJsonDocument::Compact);
        QCOMPARE(trace->dropped(), quint64(0));
    }
};
QTEST_MAIN(PerformanceClient)
#include "performance_client.moc"
