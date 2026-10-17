#include "VncWorker.h"
#include "VncAuthentication.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QElapsedTimer>

namespace {
SessionTarget targetFor(const QTcpServer& server) {
    SessionTarget target; target.protocol = "vnc"; target.host = "127.0.0.1"; target.port = server.serverPort();
    return target;
}
}
class VncWorkerTest final : public QObject {
    Q_OBJECT
private slots:
    void cleanup() { QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 5000); }
    void closeDoesNotBlockGuiOrLeaveWorkers() {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        VncWorker worker(targetFor(server), vncPasswordAuthentication(Secret(QByteArray("fixture"))), 41);
        QSignalSpy finished(&worker, &VncWorker::finished);
        int beats = 0; QTimer heartbeat; connect(&heartbeat, &QTimer::timeout, this, [&] { ++beats; }); heartbeat.start(1);
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 3000);
        auto* socket = server.nextPendingConnection(); QSignalSpy peerClosed(socket, &QTcpSocket::disconnected);
        QTRY_VERIFY_WITH_TIMEOUT(beats > 3, 1000);
        QElapsedTimer elapsed; elapsed.start(); worker.stop(); QVERIFY(elapsed.elapsed() < 100);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(peerClosed.count(), 1, 3000);
        QCOMPARE(worker.phase(), VncWorker::Phase::Cancelled);
        QVERIFY(!worker.key('a', true, 41)); QVERIFY(!worker.pointer(1, 0, 0, 41));
        worker.stop(); QCOMPARE(finished.count(), 1); socket->deleteLater();
    }
    void destroyDuringHandshakeRetiresAsynchronously() {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        auto* worker = new VncWorker(targetFor(server), vncPromptAuthentication(), 42);
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 3000);
        auto* socket = server.nextPendingConnection(); QSignalSpy peerClosed(socket, &QTcpSocket::disconnected);
        QElapsedTimer elapsed; elapsed.start(); delete worker; QVERIFY(elapsed.elapsed() < 100);
        QTRY_COMPARE_WITH_TIMEOUT(peerClosed.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 3000); socket->deleteLater();
    }
    void staleGenerationCannotSend() {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        VncWorker worker(targetFor(server), vncPromptAuthentication(), 43);
        QVERIFY(!worker.key('a', true, 42)); QVERIFY(!worker.pointer(1, 0, 0, 42));
        worker.stop(); QTRY_COMPARE_WITH_TIMEOUT(VncWorker::activeWorkers(), 0, 3000);
    }
};
QTEST_MAIN(VncWorkerTest)
#include "vnc_worker.moc"
