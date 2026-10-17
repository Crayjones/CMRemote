#include "SshWorker.h"
#include "SshAuthentication.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QElapsedTimer>

class SshWorkerTest final : public QObject {
    Q_OBJECT
private slots:
    void sshCloseDoesNotBlockGuiOrLeaveWorkers() {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        SessionTarget target; target.protocol = "ssh"; target.host = "127.0.0.1"; target.port = server.serverPort(); target.username = "literal";
        auto* worker = new SshWorker(target, sshPasswordAuthentication(Secret(QByteArray("synthetic-worker-password"))), 41);
        QSignalSpy finished(worker, &SshWorker::finished);
        int heartbeats = 0; QTimer heartbeat; connect(&heartbeat, &QTimer::timeout, this, [&] { ++heartbeats; }); heartbeat.start(1);
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 3000);
        auto* socket = server.nextPendingConnection(); QSignalSpy peerClosed(socket, &QTcpSocket::disconnected);
        QTRY_VERIFY_WITH_TIMEOUT(heartbeats > 3, 1000);
        QElapsedTimer elapsed; elapsed.start(); worker->stop(); QVERIFY(elapsed.elapsed() < 100);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(peerClosed.count(), 1, 3000);
        QCOMPARE(worker->phase(), SshWorker::Phase::Cancelled);
        QVERIFY(!worker->input(QByteArray("inactive"), 41));
        QCOMPARE(SshWorker::activeWorkers(), 0);
        delete worker; socket->deleteLater();
    }
    void sshDestroyDuringHandshakeRetiresAsynchronously() {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        SessionTarget target; target.protocol = "ssh"; target.host = "127.0.0.1"; target.port = server.serverPort(); target.username = "literal";
        auto* worker = new SshWorker(target, sshPasswordAuthentication(Secret(QByteArray("synthetic-worker-password"))), 42);
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 3000);
        auto* socket = server.nextPendingConnection(); QSignalSpy peerClosed(socket, &QTcpSocket::disconnected);
        QElapsedTimer elapsed; elapsed.start(); delete worker; QVERIFY(elapsed.elapsed() < 100);
        QTRY_COMPARE_WITH_TIMEOUT(peerClosed.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(SshWorker::activeWorkers(), 0, 3000); socket->deleteLater();
    }
    void sshStaleGenerationCannotSendOrApprove() {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        SessionTarget target; target.protocol = "ssh"; target.host = "127.0.0.1"; target.port = server.serverPort(); target.username = "literal";
        SshWorker worker(target, sshPasswordAuthentication(Secret(QByteArray("synthetic-worker-password"))), 43);
        QVERIFY(!worker.input(QByteArray("stale"), 42)); QVERIFY(!worker.resize(24, 80, 42));
        QVERIFY(!worker.answerHost("stale-token", 42, true)); QVERIFY(!worker.answerHost("stale-token", 43, true));
        worker.stop(); QTRY_COMPARE_WITH_TIMEOUT(SshWorker::activeWorkers(), 0, 3000);
    }
};
QTEST_MAIN(SshWorkerTest)
#include "ssh_worker.moc"
