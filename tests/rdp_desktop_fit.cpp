#include "RdpDesktopFit.h"
#include <QSignalSpy>
#include <QTest>

class DesktopFitTest : public QObject {
    Q_OBJECT
private slots:
    void waitsForConnectionCapabilitiesAndFrame_data() {
        QTest::addColumn<int>("order");
        for (int i = 0; i < 6; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void waitsForConnectionCapabilitiesAndFrame() {
        QFETCH(int, order);
        const int orders[][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
        RdpDesktopFit fit; QSignalSpy requests(&fit, &RdpDesktopFit::resizeRequested); fit.setViewport({1201, 701});
        for (int i = 0; i < 3; ++i) {
            switch (orders[order][i]) {
            case 0: fit.setConnected(true); break;
            case 1: fit.setAvailable(true); break;
            case 2: fit.setDesktop({1024, 768}); break;
            }
            if (i < 2) { QTest::qWait(230); QCOMPARE(requests.size(), 0); }
        }
        QTRY_COMPARE(requests.size(), 1); QCOMPARE(requests.first().first().toSize(), QSize(1200, 701));
    }
    void retriesIgnoredInitialResizeWithoutWindowMovement() {
        RdpDesktopFit fit; QSignalSpy requests(&fit, &RdpDesktopFit::resizeRequested);
        fit.setViewport({1200, 700}); fit.setConnected(true); fit.setAvailable(true); fit.setDesktop({1024, 768});
        QTRY_COMPARE(requests.size(), 1);
        // Ordinary frames at the old resolution must not postpone or cancel retry.
        fit.setDesktop({1024, 768}); fit.setViewport({1200, 700});
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 2, 2000);
        QCOMPARE(requests.last().first().toSize(), QSize(1200, 700));
        fit.setDesktop({1200, 700}); QTest::qWait(1200); QCOMPARE(requests.size(), 2);
    }
    void retriesAreBoundedAndNewViewportRearmsThem() {
        RdpDesktopFit fit; QSignalSpy requests(&fit, &RdpDesktopFit::resizeRequested);
        fit.setViewport({1200, 700}); fit.setConnected(true); fit.setAvailable(true); fit.setDesktop({1024, 768});
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 6, 8000);
        fit.setDesktop({1100, 650}); QTest::qWait(1200); QCOMPARE(requests.size(), 6);
        fit.setViewport({1300, 800}); QTRY_COMPARE(requests.size(), 7);
        QCOMPARE(requests.last().first().toSize(), QSize(1300, 800));
        fit.setConnected(false); QTest::qWait(1200); QCOMPARE(requests.size(), 7);
    }
    void latestViewportWinsAndUnavailableChannelCancels() {
        RdpDesktopFit fit; QSignalSpy requests(&fit, &RdpDesktopFit::resizeRequested);
        fit.setConnected(true); fit.setDesktop({1024, 768}); fit.setAvailable(true);
        fit.setViewport({800, 600}); fit.setViewport({1000, 700}); fit.setViewport({1401, 900});
        QTRY_COMPARE(requests.size(), 1); QCOMPARE(requests.first().first().toSize(), QSize(1400, 900));
        fit.setAvailable(false); QTest::qWait(1200); QCOMPARE(requests.size(), 1);
        fit.setAvailable(true); QTRY_COMPARE(requests.size(), 2);
        fit.setViewport({}); QTest::qWait(1200); QCOMPARE(requests.size(), 2);
    }
    void matchingDesktopAndFixedSessionsDoNotResize() {
        RdpDesktopFit fit; QSignalSpy requests(&fit, &RdpDesktopFit::resizeRequested);
        fit.setConnected(true); fit.setAvailable(true); fit.setViewport({1201, 700}); fit.setDesktop({1200, 700});
        QTest::qWait(300); QCOMPARE(requests.size(), 0);
        // Fixed resolution and SmartSize never advertise dynamic resize availability.
        fit.setAvailable(false); fit.setDesktop({800, 600}); QTest::qWait(300); QCOMPARE(requests.size(), 0);
    }
};
QTEST_GUILESS_MAIN(DesktopFitTest)
#include "rdp_desktop_fit.moc"
