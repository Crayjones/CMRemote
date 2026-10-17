#include "SessionClipboard.h"
#include <QSignalSpy>
#include <QTest>

class FakeClipboard final : public ClipboardEndpoint {
public:
    int reads = 0, writes = 0, watches = 0;
    std::optional<QString> text = QString("existing host text");
    void watch() override { ++watches; }
    std::optional<QString> readText() override { ++reads; return text; }
    void writeText(const QString& value) override { ++writes; text = value; emit changed(); }
    void copy(std::optional<QString> value) { text = std::move(value); emit changed(); }
};
class ClipboardTest : public QObject {
    Q_OBJECT
    static ClipboardStamp latest(QSignalSpy& spy) { return qvariant_cast<ClipboardControl>(spy.last()[0]).stamp; }
private slots:
    void initTestCase() { qRegisterMetaType<ClipboardControl>(); qRegisterMetaType<ClipboardStamp>(); }
    void clipboardDefaultsOffAndNeverTouchesHost() {
        auto host = std::make_shared<FakeClipboard>(); SessionClipboard session(host);
        session.setFocused(true); host->copy("new host text");
        session.receive({"remote text", {}}); session.reset();
        QVERIFY(!session.enabled()); QCOMPARE(host->reads, 0); QCOMPARE(host->writes, 0); QCOMPARE(host->watches, 0);
    }
    void clipboardSynchronizesExistingTextOnFocus() {
        auto host = std::make_shared<FakeClipboard>(); SessionClipboard session(host);
        QSignalSpy local(&session, &SessionClipboard::localText);
        session.setEnabled(true); QCOMPARE(host->reads, 0);
        session.setFocused(true); QCOMPARE(local.count(), 1); QCOMPARE(local.last()[0].toString(), *host->text);
    }
    void clipboardRoutesOnlyFocusedSession() {
        auto host = std::make_shared<FakeClipboard>(); SessionClipboard first(host), second(host);
        QSignalSpy a(&first, &SessionClipboard::localText), b(&second, &SessionClipboard::localText);
        first.setEnabled(true); second.setEnabled(true); first.setFocused(true); a.clear();
        host->copy("first copy"); QCOMPARE(a.count(), 1); QCOMPARE(b.count(), 0);
        first.setFocused(false); second.setFocused(true); a.clear(); b.clear();
        host->copy("second copy"); QCOMPARE(a.count(), 0); QCOMPARE(b.count(), 1);
        host->copy(std::nullopt); QCOMPARE(b.count(), 1);
    }
    void clipboardCopyCompletesAfterHostFocus() {
        auto host = std::make_shared<FakeClipboard>(); SessionClipboard session(host);
        QSignalSpy control(&session, &SessionClipboard::configure);
        session.setEnabled(true); session.setFocused(true); const auto stamp = latest(control);
        session.setFocused(false); session.receive({"late remote copy", stamp});
        QCOMPARE(host->writes, 1); QCOMPARE(*host->text, QString("late remote copy"));
    }
    void clipboardRemoteReplyCannotOverwriteNewerHostCopy() {
        auto host = std::make_shared<FakeClipboard>(); SessionClipboard session(host);
        QSignalSpy control(&session, &SessionClipboard::configure);
        session.setEnabled(true); session.setFocused(true); const auto stamp = latest(control);
        session.setFocused(false); host->copy("newer host copy"); session.receive({"obsolete remote copy", stamp});
        QCOMPARE(host->writes, 0); QCOMPARE(*host->text, QString("newer host copy"));
    }
    void clipboardDoesNotEchoDelayedChanges() {
        auto host = std::make_shared<FakeClipboard>(); SessionClipboard session(host);
        QSignalSpy control(&session, &SessionClipboard::configure), local(&session, &SessionClipboard::localText);
        session.setEnabled(true); session.setFocused(true); local.clear();
        session.receive({"remote copy", latest(control)}); emit host->changed(); emit host->changed();
        QCOMPARE(local.count(), 0); QCOMPARE(host->writes, 1);
        session.setFocused(false); session.setFocused(true); QCOMPARE(local.count(), 0);
        QVERIFY(host->echo(&session, "remote copy")); session.reset();
        QVERIFY(!host->echo(&session, "remote copy")); QCOMPARE(*host->text, QString("remote copy"));
        session.setEnabled(true); session.setFocused(true); local.clear();
        host->copy("different host copy"); QCOMPARE(local.count(), 1);
    }
    void clipboardDisableAndReconnectRejectPendingTransfers() {
        auto host = std::make_shared<FakeClipboard>(); SessionClipboard session(host), replacement(host);
        QSignalSpy control(&session, &SessionClipboard::configure);
        session.setEnabled(true); session.setFocused(true); const auto stamp = latest(control);
        session.reset(); const auto reads = host->reads; host->copy("host copy"); session.receive({"old", stamp});
        QCOMPARE(host->reads, reads); QCOMPARE(host->writes, 0);
        session.setEnabled(true); session.setFocused(true); session.receive({"old", stamp}); QCOMPARE(host->writes, 0);
        const auto current = latest(control); replacement.setEnabled(true); replacement.setFocused(true);
        session.receive({"other tab", current}); QCOMPARE(host->writes, 0);
    }
    void clipboardWireTextIsBoundedAndNormalized() {
        const QString text = QString::fromUtf8("empty?\nline\r\nemoji: \xf0\x9f\x98\x80\rfinal");
        auto bytes = encodeClipboardText(text); QVERIFY(bytes);
        auto decoded = decodeClipboardText(reinterpret_cast<const uchar*>(bytes->constData()), bytes->size()); QVERIFY(decoded);
        QCOMPARE(*decoded, QString::fromUtf8("empty?\nline\nemoji: \xf0\x9f\x98\x80\nfinal"));
        auto empty = encodeClipboardText({}); QVERIFY(empty); QCOMPARE(empty->size(), 2);
        QVERIFY(encodeClipboardText(QString(524287, 'x'))); QVERIFY(!encodeClipboardText(QString(524288, 'x')));
        QVERIFY(!encodeClipboardText(QString(262144, '\n')));
        QVERIFY(!encodeClipboardText(QString(QChar(0xd800)))); QVERIFY(!encodeClipboardText(QString(QChar(0xdc00))));
        QVERIFY(!encodeClipboardText(QString("a") + QChar(0) + "b"));
        for (QByteArray bad : {QByteArray("x", 1), QByteArray("xx", 2), QByteArray("x\0\0\0y\0\0\0", 8), QByteArray("\0\xd8\0\0", 4), QByteArray(1024 * 1024 + 2, '\0')})
            QVERIFY(!decodeClipboardText(reinterpret_cast<const uchar*>(bad.constData()), bad.size()));
    }
    void clipboardRequestsAreSerializedAndRevisionChecked() {
        ClipboardProtocol protocol; const ClipboardStamp first{1, 2, 3};
        protocol.control({true, true, first}); protocol.ready(); protocol.offer(true);
        QVERIFY(protocol.request()); protocol.offer(true); QVERIFY(!protocol.request());
        auto bytes = encodeClipboardText("old");
        QVERIFY(!protocol.response(reinterpret_cast<const uchar*>(bytes->constData()), bytes->size(), true));
        QVERIFY(protocol.request()); auto received = protocol.response(reinterpret_cast<const uchar*>(bytes->constData()), bytes->size(), true);
        QVERIFY(received); QCOMPARE(received->stamp, first); QVERIFY(!protocol.request());
        protocol.offer(true); QVERIFY(protocol.request()); protocol.control({true, false, first});
        QVERIFY(protocol.response(reinterpret_cast<const uchar*>(bytes->constData()), bytes->size(), true));
        protocol.control({true, true, first}); protocol.offer(true); QVERIFY(protocol.request());
        protocol.control({true, true, {1, 2, 4}}); QVERIFY(!protocol.response(reinterpret_cast<const uchar*>(bytes->constData()), bytes->size(), true));
        protocol.offer(true); QVERIFY(protocol.request()); QVERIFY(!protocol.response(nullptr, 0, false));
        protocol.offer(true); QVERIFY(protocol.request()); protocol.control({false, false, {2, 2, 4}});
        protocol.control({true, true, {3, 2, 4}}); protocol.offer(true); QVERIFY(!protocol.request());
        QVERIFY(!protocol.response(reinterpret_cast<const uchar*>(bytes->constData()), bytes->size(), true)); QVERIFY(protocol.request());
        protocol.reset(); protocol.control({true, true, first}); protocol.ready(); protocol.offer(true);
        QCOMPARE(protocol.responseWait(), qint64(-1)); QVERIFY(protocol.request(0)); QCOMPARE(protocol.responseWait(), qint64(0));
        protocol.offer(true); QVERIFY(!protocol.request());
        protocol.reset(); QCOMPARE(protocol.responseWait(), qint64(-1));
    }
    void clipboardRemoteOwnershipDoesNotWithdrawImportedText() {
        ClipboardProtocol protocol; const ClipboardStamp first{1, 1, 1};
        protocol.control({true, true, first}); protocol.ready(); protocol.local("host copy", first);
        QCOMPARE(protocol.advertisement(), std::optional<bool>(true));
        protocol.offer(true); QVERIFY(protocol.request()); auto bytes = encodeClipboardText("remote copy");
        QVERIFY(protocol.response(reinterpret_cast<const uchar*>(bytes->constData()), bytes->size(), true));
        protocol.control({true, true, {1, 1, 2}});
        QVERIFY(!protocol.localReply()); QVERIFY(!protocol.advertisement());
        protocol.local("new host copy", {1, 1, 2}); QCOMPARE(protocol.advertisement(), std::optional<bool>(true));
    }
    void clipboardDisabledRejectsDataRequests() {
        ClipboardProtocol protocol; protocol.ready(); protocol.offer(true);
        QVERIFY(!protocol.request()); QVERIFY(!protocol.localReply()); QVERIFY(!protocol.advertisement());
        protocol.control({true, true, {1, 1, 1}}); protocol.local("host", {1, 1, 1}); QVERIFY(protocol.localReply());
        QCOMPARE(protocol.advertisement(), std::optional<bool>(true));
        protocol.control({true, false, {1, 1, 1}}); QVERIFY(!protocol.localReply());
        protocol.control({true, true, {1, 1, 1}}); QVERIFY(protocol.localReply());
        protocol.control({false, false, {2, 1, 1}}); QVERIFY(!protocol.localReply());
        QCOMPARE(protocol.advertisement(), std::optional<bool>(false));
        protocol.offer(true); QVERIFY(!protocol.request());
    }
};
QTEST_GUILESS_MAIN(ClipboardTest)
#include "session_clipboard.moc"
