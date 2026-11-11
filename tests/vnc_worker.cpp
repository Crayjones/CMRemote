#include "VncWorker.h"
#include "VncAuthentication.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QElapsedTimer>
#include <QtEndian>
#include <algorithm>

namespace {
// RFB 3.8 with None security and a 64x48 desktop; records the client's pointer events.
class PointerServer final : public QObject {
public:
    struct Pointer { quint8 mask; quint16 x, y; };
    QTcpServer server; QList<Pointer> pointers;
    PointerServer() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            socket_ = server.nextPendingConnection(); socket_->write("RFB 003.008\n");
            connect(socket_, &QTcpSocket::readyRead, this, [this] { buffer_ += socket_->readAll(); parse(); });
        });
    }
private:
    void parse() {
        const auto take = [this](int size) { auto bytes = buffer_.left(size); buffer_.remove(0, size); return bytes; };
        while (true) {
            if (stage_ == 0) { if (buffer_.size() < 12) return; take(12); socket_->write(QByteArray("\x01\x01", 2)); stage_ = 1; }
            else if (stage_ == 1) { if (buffer_.size() < 1) return; take(1); socket_->write(QByteArray(4, '\0')); stage_ = 2; }
            else if (stage_ == 2) {
                if (buffer_.size() < 1) return; take(1);
                const char init[] = {0, 64, 0, 48, 32, 24, 0, 1, 0, char(255), 0, char(255), 0, char(255), 16, 8, 0, 0, 0, 0, 0, 0, 0, 1, 'p'};
                socket_->write(init, sizeof(init)); stage_ = 3;
            } else {
                if (buffer_.isEmpty()) return;
                const auto type = quint8(buffer_[0]); int size = 0;
                if (type == 0) size = 20; else if (type == 3) size = 10; else if (type == 4) size = 8; else if (type == 5) size = 6;
                else if (type == 2) { if (buffer_.size() < 4) return; size = 4 + 4 * ((quint8(buffer_[2]) << 8) | quint8(buffer_[3])); }
                else if (type == 6) { if (buffer_.size() < 8) return; size = 8 + int(qFromBigEndian<quint32>(buffer_.constData() + 4)); }
                else return;
                if (buffer_.size() < size) return;
                const auto message = take(size);
                if (type == 5) pointers.append({quint8(message[1]), qFromBigEndian<quint16>(message.constData() + 2), qFromBigEndian<quint16>(message.constData() + 4)});
            }
        }
    }
    QTcpSocket* socket_ = nullptr; QByteArray buffer_; int stage_ = 0;
};
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
    // A press followed at once by a drag with the same buttons held must reach the server as
    // a press at its own position; only consecutive motion is coalesced.
    void buttonTransitionsAreNeverCoalescedWithMotion() {
        PointerServer fake; QVERIFY(fake.server.isListening());
        VncWorker worker(targetFor(fake.server), vncPromptAuthentication(), 44);
        QTRY_COMPARE_WITH_TIMEOUT(worker.phase(), VncWorker::Phase::Connected, 5000);
        QVERIFY(worker.pointer(0, 1, 1, 44)); QVERIFY(worker.pointer(1, 10, 10, 44));
        for (quint16 x = 11; x < 40; ++x) QVERIFY(worker.pointer(1, x, 20, 44));
        QVERIFY(worker.pointer(0, 40, 20, 44));
        QTRY_VERIFY_WITH_TIMEOUT(!fake.pointers.isEmpty() && fake.pointers.last().mask == 0 && fake.pointers.last().x == 40, 3000);
        const auto pressed = std::find_if(fake.pointers.begin(), fake.pointers.end(), [](const auto& event) { return event.mask == 1; });
        QVERIFY(pressed != fake.pointers.end()); QCOMPARE(pressed->x, quint16(10)); QCOMPARE(pressed->y, quint16(10));
        worker.stop();
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
