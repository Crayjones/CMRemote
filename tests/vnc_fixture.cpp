// Opt-in client acceptance against the owned TigerVNC fixture (scripts/vnc-fixture.py).
// Run with: python3 scripts/vnc-fixture.py client. Never part of ordinary CTest.
#include "VncWorker.h"
#include "VncCertificates.h"
#include <QDir>
#include <QElapsedTimer>
#include <QSet>
#include <QFile>
#include <QSignalSpy>
#include <QTest>

namespace {
struct Fixture { QByteArray password; QString ca; quint16 base = 0; };
Fixture fixture() {
    Fixture result;
    result.password = qgetenv("RM_VNC_FIXTURE_PASSWORD");
    result.ca = qEnvironmentVariable("RM_VNC_FIXTURE_CA");
    result.base = quint16(qEnvironmentVariableIntValue("RM_VNC_FIXTURE_PORT"));
    return result;
}
SessionTarget target(quint16 port) {
    SessionTarget value; value.protocol = "vnc"; value.host = "127.0.0.1"; value.port = port; return value;
}
VncAuthentication saved(const QByteArray& password) { return vncPasswordAuthentication(Secret(password)); }
// The fixture CA imported, as Settings → Remote Desktop would.
VncWorker::Options trusted(const QString& ca, QString username = {}) {
    QFile file(ca); file.open(QIODevice::ReadOnly);
    return {readVncCertificates(file.readAll()), std::nullopt, std::move(username)};
}
// Applies the worker's updates to desktop for a while; returns the latest picture.
void collect(VncWorker& worker, QImage& desktop, int milliseconds) {
    QElapsedTimer elapsed; elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
        QTest::qWait(30);
        auto update = worker.takeUpdate();
        if (update.patches.empty()) continue;
        if (desktop.size() != update.size) { desktop = QImage(update.size, QImage::Format_RGB32); desktop.fill(Qt::black); }
        if (!applyDesktopUpdate(desktop, update)) { desktop = {}; return; }
    }
}
bool waitPhase(VncWorker& worker, std::initializer_list<VncWorker::Phase> phases, int timeout = 20000) {
    QElapsedTimer elapsed; elapsed.start();
    while (elapsed.elapsed() < timeout) {
        for (auto phase : phases) if (worker.phase() == phase) return true;
        QTest::qWait(20);
    }
    return false;
}
}

class VncFixtureTest : public QObject {
    Q_OBJECT
    Fixture fixture_ = fixture();
private slots:
    void initTestCase() {
        if (fixture_.password.isEmpty() || !fixture_.base) QSKIP("Run through: python3 scripts/vnc-fixture.py client");
        QVERIFY(QFile::exists(fixture_.ca));
    }
    void cleanup() { QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 10000); }
    void everySecurityTypeConnectsDrawsResizesAndTakesInput_data() {
        QTest::addColumn<int>("offset"); QTest::addColumn<QString>("security"); QTest::addColumn<bool>("encrypted"); QTest::addColumn<QString>("identity");
        QTest::newRow("VncAuth") << 0 << "VncAuth" << false << "";
        QTest::newRow("Plain") << 1 << "VeNCrypt Plain" << false << "";
        QTest::newRow("TLSPlain") << 2 << "VeNCrypt TLSPlain" << true << "";
        QTest::newRow("X509Plain") << 3 << "VeNCrypt X509Plain" << true << "certificate verified";
        QTest::newRow("TLSVnc") << 4 << "VeNCrypt TLSVnc" << true << "";
        QTest::newRow("X509Vnc") << 5 << "VeNCrypt X509Vnc" << true << "certificate verified";
        // Every type offered at once: the client prefers encrypted, verified types.
        QTest::newRow("all") << 6 << "VeNCrypt X509Plain" << true << "certificate verified";
    }
    void everySecurityTypeConnectsDrawsResizesAndTakesInput() {
        QFETCH(int, offset); QFETCH(QString, security); QFETCH(bool, encrypted); QFETCH(QString, identity);
        VncWorker worker(target(fixture_.base + offset), saved(fixture_.password), 1, trusted(fixture_.ca, "vncfixture"));
        QSignalSpy frames(&worker, &VncWorker::frameAvailable), resizable(&worker, &VncWorker::resizeAvailable);
        QVERIFY2(waitPhase(worker, {VncWorker::Phase::Connected, VncWorker::Phase::Failed, VncWorker::Phase::PasswordRequired}),
                 qPrintable(QString("phase %1").arg(int(worker.phase()))));
        QSignalSpy failed(&worker, &VncWorker::phaseChanged);
        QCOMPARE(worker.phase(), VncWorker::Phase::Connected);
        QCOMPARE(worker.security(), security); QCOMPARE(worker.encrypted(), encrypted); QCOMPARE(worker.identity(), identity);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 10000);
        auto update = worker.takeUpdate(); QVERIFY(!update.patches.empty()); QVERIFY(update.size.width() > 0);
        QTRY_VERIFY_WITH_TIMEOUT(!resizable.isEmpty(), 10000); QCOMPARE(resizable.last().at(0).toBool(), true);
        // The server desktop follows a requested size (RDP-style automatic resize).
        worker.resizeDesktop(QSize(1000, 700));
        QElapsedTimer elapsed; elapsed.start(); QSize size = update.size;
        while (size != QSize(1000, 700) && elapsed.elapsed() < 10000) {
            QTest::qWait(50); if (auto next = worker.takeUpdate(); !next.patches.empty()) size = next.size;
        }
        QCOMPARE(size, QSize(1000, 700));
        QString error;
        QVERIFY2(worker.pointer(0, 500, 350, 1, &error), qPrintable(error));
        QVERIFY2(worker.key(0x61, true, 1, &error) && worker.key(0x61, false, 1, &error), qPrintable(error));
        worker.configureClipboard({true, true, {1, 1, 1}});
        worker.clipboardText(QString::fromUtf8("CMRemote clipboard é"), {1, 1, 1});
        QTest::qWait(300);
        QCOMPARE(worker.phase(), VncWorker::Phase::Connected);
        QSignalSpy finished(&worker, &VncWorker::finished);
        worker.stop(); QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 10000);
        QCOMPARE(worker.phase(), VncWorker::Phase::Cancelled);
    }
    // Every display runs an XFCE desktop; a near-uniform frame means the session did not start.
    // Screenshots land in build/<platform>/test-data/vnc-fixture/ for a manual look.
    void everyPortShowsADesktop() {
        const auto directory = qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/vnc-fixture";
        QVERIFY(QDir().mkpath(directory));
        for (int offset = 0; offset < 7; ++offset) {
            VncWorker worker(target(fixture_.base + offset), saved(fixture_.password), 1, trusted(fixture_.ca, "vncfixture"));
            QVERIFY(waitPhase(worker, {VncWorker::Phase::Connected, VncWorker::Phase::Failed}));
            QCOMPARE(worker.phase(), VncWorker::Phase::Connected);
            QImage desktop; collect(worker, desktop, 1500);
            QVERIFY(!desktop.isNull());
            QVERIFY(desktop.save(directory + QString("/port-%1.png").arg(fixture_.base + offset)));
            QSet<QRgb> colors;
            for (int y = 0; y < desktop.height(); y += 7) for (int x = 0; x < desktop.width(); x += 7) colors.insert(desktop.pixel(x, y));
            QVERIFY2(colors.size() > 64, qPrintable(QString("port %1 shows only %2 colors").arg(fixture_.base + offset).arg(colors.size())));
            worker.stop();
            QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 10000);
        }
    }
    void wrongPasswordIsRejected_data() {
        QTest::addColumn<int>("offset"); QTest::addColumn<QString>("message");
        QTest::newRow("VncAuth") << 0 << "rejected the password";
        QTest::newRow("Plain") << 1 << "rejected the username or password";
        QTest::newRow("TLSVnc") << 4 << "rejected the password";
        QTest::newRow("X509Plain") << 3 << "rejected the username or password";
    }
    void wrongPasswordIsRejected() {
        QFETCH(int, offset); QFETCH(QString, message);
        VncWorker worker(target(fixture_.base + offset), saved("wrong-password"), 1, trusted(fixture_.ca, "vncfixture"));
        QSignalSpy phases(&worker, &VncWorker::phaseChanged), finished(&worker, &VncWorker::finished);
        QVERIFY(waitPhase(worker, {VncWorker::Phase::Failed, VncWorker::Phase::Connected}));
        QCOMPARE(worker.phase(), VncWorker::Phase::Failed);
        QVERIFY2(phases.last().at(1).toString().contains(message), qPrintable(phases.last().at(1).toString()));
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 10000);
    }
    void promptsForMissingCredentials_data() {
        QTest::addColumn<int>("offset"); QTest::addColumn<bool>("username");
        QTest::newRow("VncAuth") << 0 << false;
        QTest::newRow("Plain") << 1 << true;
        QTest::newRow("TLSPlain") << 2 << true;
    }
    void promptsForMissingCredentials() {
        QFETCH(int, offset); QFETCH(bool, username);
        VncWorker worker(target(fixture_.base + offset), vncPromptAuthentication(), 1, trusted(fixture_.ca));
        QVERIFY(waitPhase(worker, {VncWorker::Phase::PasswordRequired, VncWorker::Phase::Failed}));
        QCOMPARE(worker.phase(), VncWorker::Phase::PasswordRequired);
        QCOMPARE(worker.prompt(), username ? VncWorker::Prompt::UsernamePassword : VncWorker::Prompt::Password);
        // The prompt does not count toward the 30-second handshake limit, and Plain needs a username.
        QString error;
        if (username) QVERIFY(!worker.credentials({}, Secret(fixture_.password), 1, &error));
        QVERIFY2(worker.credentials(username ? "vncfixture" : QString(), Secret(fixture_.password), 1, &error), qPrintable(error));
        QVERIFY(waitPhase(worker, {VncWorker::Phase::Connected, VncWorker::Phase::Failed}));
        QCOMPARE(worker.phase(), VncWorker::Phase::Connected);
        worker.stop();
    }
    // Without the CA the certificate is reviewed; Cancel fails, Connect once connects, and a
    // pinned certificate connects without asking until it no longer matches.
    void unverifiedCertificateIsReviewedAndCanBePinned() {
        VncCertificateReview review;
        {
            VncWorker worker(target(fixture_.base + 5), saved(fixture_.password), 1, {});
            QSignalSpy phases(&worker, &VncWorker::phaseChanged);
            QVERIFY(waitPhase(worker, {VncWorker::Phase::CertificateReview, VncWorker::Phase::Failed, VncWorker::Phase::Connected}));
            QCOMPARE(worker.phase(), VncWorker::Phase::CertificateReview);
            review = worker.certificateReview();
            QCOMPARE(review.host, QString("127.0.0.1")); QCOMPARE(review.port, quint16(fixture_.base + 5)); QVERIFY(!review.previous);
            QVERIFY2(!review.reason.isEmpty(), "A review explains why verification failed");
            QVERIFY(review.certificate.subject.contains("CN=localhost")); QCOMPARE(review.certificate.sha256.size(), 32);
            QVERIFY(review.certificate.issuer.contains("RemoteManager VNC fixture CA"));
            QTest::qWait(200); QCOMPARE(worker.phase(), VncWorker::Phase::CertificateReview); // waits for the user
            QVERIFY(worker.trustCertificate(VncWorker::CertificateTrust::Reject, 1));
            QVERIFY(waitPhase(worker, {VncWorker::Phase::Failed, VncWorker::Phase::Connected}));
            QCOMPARE(worker.phase(), VncWorker::Phase::Failed);
            QVERIFY2(phases.last().at(1).toString().contains("not trusted"), qPrintable(phases.last().at(1).toString()));
        }
        QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 10000);
        {
            VncWorker worker(target(fixture_.base + 5), saved(fixture_.password), 1, {});
            QVERIFY(waitPhase(worker, {VncWorker::Phase::CertificateReview}));
            QVERIFY(worker.trustCertificate(VncWorker::CertificateTrust::Once, 1));
            QVERIFY(waitPhase(worker, {VncWorker::Phase::Connected, VncWorker::Phase::Failed}));
            QCOMPARE(worker.phase(), VncWorker::Phase::Connected); QCOMPARE(worker.identity(), QString("certificate accepted for this session"));
            worker.stop();
        }
        QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 10000);
        {
            VncWorker worker(target(fixture_.base + 3), saved(fixture_.password), 1, {{}, review.certificate, "vncfixture"});
            QVERIFY(waitPhase(worker, {VncWorker::Phase::Connected, VncWorker::Phase::Failed, VncWorker::Phase::CertificateReview}));
            QCOMPARE(worker.phase(), VncWorker::Phase::Connected); QCOMPARE(worker.identity(), QString("trusted certificate"));
            worker.stop();
        }
        QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 10000);
        {
            // A different pinned certificate (here the CA's) is a changed certificate.
            QFile ca(fixture_.ca); QVERIFY(ca.open(QIODevice::ReadOnly));
            const auto other = describeVncCertificate(readVncCertificates(ca.readAll()).value(0)); QVERIFY(other);
            VncWorker worker(target(fixture_.base + 5), saved(fixture_.password), 1, {{}, *other, {}});
            QVERIFY(waitPhase(worker, {VncWorker::Phase::CertificateReview, VncWorker::Phase::Connected, VncWorker::Phase::Failed}));
            QCOMPARE(worker.phase(), VncWorker::Phase::CertificateReview);
            QVERIFY(worker.certificateReview().previous); QCOMPARE(worker.certificateReview().previous->sha256, other->sha256);
            worker.stop();
        }
    }
    void verifiedCertificateMustNameTheHost() {
        // "localhost" is in the certificate as well; an address it does not list is reviewed.
        auto host = target(fixture_.base + 5); host.host = "localhost";
        VncWorker named(host, saved(fixture_.password), 1, trusted(fixture_.ca));
        QVERIFY(waitPhase(named, {VncWorker::Phase::Connected, VncWorker::Phase::Failed, VncWorker::Phase::CertificateReview}));
        QCOMPARE(named.phase(), VncWorker::Phase::Connected); QCOMPARE(named.identity(), QString("certificate verified"));
        named.stop();
        auto other = target(fixture_.base + 5); other.host = "::ffff:127.0.0.1"; // the same server, an address the certificate does not list
        VncWorker mismatched(other, saved(fixture_.password), 1, trusted(fixture_.ca));
        QVERIFY(waitPhase(mismatched, {VncWorker::Phase::CertificateReview, VncWorker::Phase::Connected, VncWorker::Phase::Failed}));
        QCOMPARE(mismatched.phase(), VncWorker::Phase::CertificateReview);
        QVERIFY2(mismatched.certificateReview().reason.contains("mismatch", Qt::CaseInsensitive), qPrintable(mismatched.certificateReview().reason));
        mismatched.stop();
    }
    // xfwm4 tiles a window dragged to the left edge and maximizes one dragged to the top,
    // which needs the drag to reach the screen edge through the client.
    void windowDraggedToAnEdgeTiles() {
        VncWorker worker(target(fixture_.base + 4), saved(fixture_.password), 1, trusted(fixture_.ca));
        QVERIFY(waitPhase(worker, {VncWorker::Phase::Connected, VncWorker::Phase::Failed}));
        QCOMPARE(worker.phase(), VncWorker::Phase::Connected);
        QImage desktop; collect(worker, desktop, 1500); QVERIFY(!desktop.isNull());
        const int width = desktop.width(), height = desktop.height();
        const auto terminal = [&](int x, int y) { return desktop.pixelColor(x, y) == QColor(Qt::black); };
        QVERIFY2(!terminal(20, height / 2), "the terminal starts away from the left edge");
        QString error;
        const auto drag = [&](QPoint from, QPoint to) {
            QVERIFY(worker.pointer(0, quint16(from.x()), quint16(from.y()), 1, &error)); QTest::qWait(100);
            QVERIFY(worker.pointer(1, quint16(from.x()), quint16(from.y()), 1, &error)); QTest::qWait(100);
            for (int step = 1; step <= 20; ++step) {
                const auto point = from + (to - from) * step / 20;
                QVERIFY(worker.pointer(1, quint16(point.x()), quint16(point.y()), 1, &error)); QTest::qWait(25);
            }
            QTest::qWait(400);
            QVERIFY(worker.pointer(0, quint16(to.x()), quint16(to.y()), 1, &error));
            collect(worker, desktop, 1200);
        };
        drag({300, 75}, {0, height / 2});
        desktop.save(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/vnc-fixture/tiled-left.png");
        QVERIFY2(terminal(20, height / 2) && !terminal(width - 40, height / 2), "dragged to the left edge, the window fills the left half");
        // Its title bar is now at the top of the left half; drag it to the top edge.
        drag({width / 4, 40}, {width / 2, 0});
        desktop.save(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/vnc-fixture/maximized.png");
        QVERIFY2(terminal(20, height / 2) && terminal(width - 60, height / 2), "dragged to the top edge, the window is maximized");
        // Dragging the maximized window down restores it to a floating window.
        drag({width / 2, 37}, {width / 2, height / 3});
        QVERIFY2(!terminal(width - 10, height / 2) || !terminal(10, height / 2), "dragged down, the window is no longer maximized");
        worker.stop();
    }
    // Highlighting text on the remote desktop (X11 PRIMARY) must not reach this computer's
    // clipboard; an explicit copy (CLIPBOARD) does. The fixture runs Xvnc with SendPrimary=0.
    void onlyAnExplicitRemoteCopyReachesTheClipboard() {
        VncWorker worker(target(fixture_.base + 1), saved(fixture_.password), 1, trusted(fixture_.ca, "vncfixture"));
        QVERIFY(waitPhase(worker, {VncWorker::Phase::Connected, VncWorker::Phase::Failed}));
        QCOMPARE(worker.phase(), VncWorker::Phase::Connected);
        worker.configureClipboard({true, true, {1, 1, 1}});
        QSignalSpy received(&worker, &VncWorker::clipboardAvailable);
        QImage desktop; collect(worker, desktop, 1000);
        QString error;
        // Select the terminal's first line ("Security: Plain ...") by dragging across it.
        QVERIFY(worker.pointer(0, 70, 124, 1, &error)); QTest::qWait(100);
        QVERIFY(worker.pointer(1, 70, 124, 1, &error)); QTest::qWait(100);
        for (int x = 80; x <= 400; x += 20) { QVERIFY(worker.pointer(1, quint16(x), 124, 1, &error)); QTest::qWait(20); }
        QVERIFY(worker.pointer(0, 400, 124, 1, &error));
        QTest::qWait(1500); worker.takeClipboard();
        QVERIFY2(received.isEmpty(), "a selection alone must not copy");
        // xfce4-terminal copies with Ctrl+Shift+C.
        for (const quint32 key : {0xffe3u, 0xffe1u, 0x43u}) QVERIFY(worker.key(key, true, 1, &error));
        for (const quint32 key : {0x43u, 0xffe1u, 0xffe3u}) QVERIFY(worker.key(key, false, 1, &error));
        QTRY_VERIFY_WITH_TIMEOUT(!received.isEmpty(), 5000);
        const auto delivery = worker.takeClipboard(); QVERIFY(delivery);
        QVERIFY2(delivery->text.contains("Security"), qPrintable(delivery->text));
        worker.stop();
    }
    void cancelDuringPromptRetires() {
        auto* worker = new VncWorker(target(fixture_.base + 1), vncPromptAuthentication(), 1, trusted(fixture_.ca));
        QVERIFY(waitPhase(*worker, {VncWorker::Phase::PasswordRequired}));
        QElapsedTimer elapsed; elapsed.start(); delete worker; QVERIFY(elapsed.elapsed() < 200);
    }
};
QTEST_MAIN(VncFixtureTest)
#include "vnc_fixture.moc"
