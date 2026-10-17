#include "DataPaths.h"
#include "DocumentService.h"
#include "MainWindow.h"
#include <QApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

static QByteArray contents(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
static bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
class XmlStorageTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qApp->setQuitOnLastWindowClosed(false); }
    void defaultStartupUsesXmlV73() {
        QTemporaryDir dir; const auto xml = dir.filePath("confCons.xml");
        QCOMPARE(applicationConnectionPath(dir.path()), xml);
        const auto other = dir.filePath(QStringLiteral("profiles") + ".json");
        const QByteArray unsupported = "{\"schema_version\":1,\"profiles\":[]}";
        QVERIFY(write(other, unsupported));
        QCOMPARE(applicationConnectionPath(dir.path()), xml);
        MainWindow window(applicationConnectionPath(dir.path()), true);
        QTRY_COMPARE(window.connectionFiles().size(), 1);
        const auto id = window.connectionFiles().first();
        QTRY_VERIFY(window.findChild<DocumentService*>()->status(id)->metadataAvailable);
        QVERIFY(!QFile::exists(xml)); QCOMPARE(contents(other), unsupported);
    }
    void nonXmlOpenRejectsWithoutMutationV73_data() {
        QTest::addColumn<QByteArray>("input");
        QTest::newRow("structured-text") << QByteArray("{\"schema_version\":1,\"profiles\":[]}");
        QTest::newRow("binary") << QByteArray::fromHex("524d434f4e53000000000001");
        QTest::newRow("damaged-xml") << QByteArray("<Connections>");
    }
    void nonXmlOpenRejectsWithoutMutationV73() {
        QFETCH(QByteArray, input); QTemporaryDir dir; const auto path = dir.filePath("input.dat"); QVERIFY(write(path, input));
        DocumentService service; bool ready = false, accepted = true; QByteArray metadata;
        service.open(path, [&](DocumentReply reply) { accepted = reply.result.ok; metadata = reply.projection; ready = true; });
        QTRY_VERIFY(ready); QVERIFY(!accepted); QVERIFY(metadata.isEmpty()); QCOMPARE(contents(path), input);
    }
    void everyShellUsesXmlV73_data() {
        QTest::addColumn<bool>("mock"); QTest::newRow("production") << false; QTest::newRow("mock") << true;
    }
    void everyShellUsesXmlV73() {
        QFETCH(bool, mock); QTemporaryDir dir; const auto path = dir.filePath("connections.data");
        const Secret master(QByteArray("mR3m")); auto xml = ConnectionXml::create(); QVERIFY(xml);
        const auto bytes = xml->serialize(master); QVERIFY(bytes); QVERIFY(write(path, *bytes));
        MainWindow window(path, mock);
        QTRY_COMPARE(window.connectionFiles().size(), 1);
        const auto id = window.connectionFiles().first();
        auto* service = window.findChild<DocumentService*>(); QVERIFY(service);
        QTRY_VERIFY(service->status(id)->metadataAvailable); QVERIFY(service->status(id)->unlocked); QCOMPARE(contents(path), *bytes);
    }
};
QTEST_MAIN(XmlStorageTest)
#include "xml_storage.moc"
