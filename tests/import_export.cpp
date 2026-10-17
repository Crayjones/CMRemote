#include "ConnectionImport.h"
#include "ConnectionXml.h"
#include "ExternalTools.h"
#include "GridDriver.h"
#include "MainWindow.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringEncoder>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#define WAIT_STORE(window) do { QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open); QVERIFY(waitGridSaved(window)); } while (false)

namespace {
struct Read { QJsonArray profiles; ImportedPasswords passwords; };
// Reads converted output exactly as the importer does: native reader, default key.
std::optional<Read> read(const QByteArray& bytes) {
    auto xml = ConnectionXml::read(bytes); const Secret key(QByteArray("mR3m"));
    if (!xml || !xml->unlock(key)) return std::nullopt;
    auto projection = xml->projection(); auto passwords = xml->importPasswords(key);
    if (!projection || !passwords) return std::nullopt;
    return Read{QJsonDocument::fromJson(*projection).object().value("profiles").toArray(), std::move(*passwords)};
}
QJsonObject named(const QJsonArray& profiles, const QString& name) {
    for (const auto& value : profiles) if (value.toObject().value("name") == name) return value.toObject();
    return {};
}
QString text(const Secret& secret) { return QString::fromUtf8(QByteArray(reinterpret_cast<const char*>(secret.data()), qsizetype(secret.size()))); }
}

class ImportExportTest : public QObject {
    Q_OBJECT
private slots:
    void rdpFileIncludingUtf16() {
        auto encode = QStringEncoder(QStringEncoder::Utf16LE);
        const QByteArray bytes = QByteArray("\xFF\xFE") + QByteArray(encode(QString("full address:s:rdp.example:3390\r\nusername:s:CORP\\alice\r\nadministrative session:i:1\r\n")));
        QString error; const auto xml = ConnectionImport::fromFile("/tmp/Office PC.rdp", bytes, &error); QVERIFY2(xml, qPrintable(error));
        const auto result = read(*xml); QVERIFY(result); QCOMPARE(result->profiles.size(), 1);
        const auto overrides = named(result->profiles, "Office PC").value("overrides").toObject();
        QCOMPARE(overrides.value("host").toString(), QString("rdp.example")); QCOMPARE(overrides.value("port").toInt(), 3390);
        QCOMPARE(overrides.value("username").toString(), QString("alice")); QCOMPARE(overrides.value("domain").toString(), QString("CORP"));
        QCOMPARE(overrides.value("protocol").toString(), QString("rdp")); QCOMPARE(overrides.value("console_session").toBool(), true);
    }
    void mremotengCsvWithHierarchyAndQuotes() {
        const QByteArray bytes = "Name;Id;Parent;NodeType;Description;Username;Password;Hostname;Port;Protocol\r\n"
                                 "Servers;f1;;Container;;;;;;\r\n"
                                 "\"Web; front\";c1;f1;Connection;\"Says \"\"hi\"\"\";root;s3cret;web.example;22;SSH2\r\n";
        QString error; const auto xml = ConnectionImport::fromFile("list.csv", bytes, &error); QVERIFY2(xml, qPrintable(error));
        const auto result = read(*xml); QVERIFY(result); QCOMPARE(result->profiles.size(), 2);
        const auto folder = named(result->profiles, "Servers"), web = named(result->profiles, "Web; front");
        QCOMPARE(folder.value("kind").toString(), QString("folder")); QCOMPARE(web.value("parent").toString(), folder.value("id").toString());
        QCOMPARE(web.value("overrides").toObject().value("description").toString(), QString("Says \"hi\""));
        QCOMPARE(web.value("overrides").toObject().value("protocol").toString(), QString("ssh"));
        QCOMPARE(text(result->passwords.local.at(web.value("id").toString())), QString("s3cret"));
    }
    void rdcmanGroupsAndServers() {
        const QByteArray bytes = "<RDCMan programVersion=\"2.7\" schemaVersion=\"3\"><file><properties><name>Lab</name></properties>"
            "<group><properties><name>DCs</name></properties><server><properties><displayName>DC 1</displayName><name>dc1.lab</name></properties>"
            "<logonCredentials inherit=\"None\"><userName>admin</userName><domain>LAB</domain></logonCredentials>"
            "<connectionSettings inherit=\"None\"><port>3391</port></connectionSettings></server></group></file></RDCMan>";
        QString error; const auto xml = ConnectionImport::fromFile("lab.rdg", bytes, &error); QVERIFY2(xml, qPrintable(error));
        const auto result = read(*xml); QVERIFY(result); QCOMPARE(result->profiles.size(), 3);
        const auto server = named(result->profiles, "DC 1"); QCOMPARE(named(result->profiles, "DCs").value("parent").toString(), named(result->profiles, "Lab").value("id").toString());
        QCOMPARE(server.value("overrides").toObject().value("host").toString(), QString("dc1.lab"));
        QCOMPARE(server.value("overrides").toObject().value("port").toInt(), 3391);
        QCOMPARE(server.value("overrides").toObject().value("domain").toString(), QString("LAB"));
    }
    void puttyConnectionManager() {
        const QByteArray bytes = "<?xml version=\"1.0\"?><configuration version=\"0.7\"><root type=\"database\" name=\"Mine\">"
            "<container type=\"folder\" name=\"Linux\"><connection type=\"PuTTY\" name=\"box\"><connection_info><protocol>SSH</protocol>"
            "<host>box.example</host><port>2200</port></connection_info><login><login>me</login><password>pw</password></login>"
            "</connection></container></root></configuration>";
        QString error; const auto xml = ConnectionImport::fromFile("cm.dat", bytes, &error); QVERIFY2(xml, qPrintable(error));
        const auto result = read(*xml); QVERIFY(result); QCOMPARE(result->profiles.size(), 3);
        const auto box = named(result->profiles, "box");
        QCOMPARE(box.value("overrides").toObject().value("protocol").toString(), QString("ssh"));
        QCOMPARE(box.value("overrides").toObject().value("port").toInt(), 2200);
        QCOMPARE(text(result->passwords.local.at(box.value("id").toString())), QString("pw"));
    }
    void rejectsInvalidInput() {
        QString error;
        QVERIFY(!ConnectionImport::fromFile("x.rdg", "<notrdcman/>", &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!ConnectionImport::fromFile("x.csv", "Hostname\r\nhost\r\n", &error));
        QVERIFY(!ConnectionImport::fromFile("x.rdp", "username:s:alice\r\n", &error));
    }
    void importIntoFolderThenExportXmlAndCsv() {
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/import-export-XXXXXX"); QVERIFY(dir.isValid());
        MainWindow window(dir.filePath("confCons.xml"), true); window.show(); WAIT_STORE(window);
        window.addFolder(); WAIT_STORE(window); const auto folder = window.selectedId();
        QFile csv(dir.filePath("list.csv")); QVERIFY(csv.open(QIODevice::WriteOnly));
        csv.write("Name;Username;Password;Domain;Hostname;Port;Protocol\r\nalpha;alice;pa55;CORP;alpha.example;3389;RDP\r\n"); csv.close();
        window.importConnections(csv.fileName()); WAIT_STORE(window);
        QTRY_VERIFY(window.lastError().isEmpty());
        auto* tree = window.findChild<QTreeWidget*>("connectionTree");
        // The tree is rebuilt after each write, so look the folder up each time.
        const auto children = [tree] {
            QStringList names;
            for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->text(0) == "New folder") for (int i = 0; i < (*it)->childCount(); ++i) names << (*it)->child(i)->text(0);
            return names;
        };
        QTRY_COMPARE(children(), QStringList({"alpha"}));
        // Importing the same file again adds a second copy rather than colliding.
        window.selectConnectionProfile({window.selectedDocument(), folder});
        window.importConnections(csv.fileName()); WAIT_STORE(window); QTRY_COMPARE(children(), QStringList({"alpha", "alpha"}));

        window.selectConnectionFile(window.selectedDocument());
        ExportOptions everything; const auto xmlPath = dir.filePath("export.xml");
        window.exportConnections(xmlPath, everything); QTRY_VERIFY(QFile::exists(xmlPath));
        QFile exported(xmlPath); QVERIFY(exported.open(QIODevice::ReadOnly)); const auto bytes = exported.readAll();
        QVERIFY(!bytes.contains("RmFileId")); QVERIFY(!bytes.contains("RmCredentialBindings"));
        const auto result = read(bytes); QVERIFY(result); QCOMPARE(result->profiles.size(), 3);
        const auto alpha = named(result->profiles, "alpha");
        QCOMPARE(text(result->passwords.local.at(alpha.value("id").toString())), QString("pa55"));

        ExportOptions csvOptions; csvOptions.format = ExportOptions::Format::Csv; csvOptions.password = false; csvOptions.domain = false;
        const auto csvPath = dir.filePath("export.csv");
        window.exportConnections(csvPath, csvOptions); QTRY_VERIFY(QFile::exists(csvPath));
        QFile csvOut(csvPath); QVERIFY(csvOut.open(QIODevice::ReadOnly)); const auto csvText = QString::fromUtf8(csvOut.readAll());
        QVERIFY(csvText.startsWith("Name;Id;Parent;NodeType;Description;Icon;Panel;Username;Password;Domain;Hostname;Port;Protocol"));
        QVERIFY(csvText.contains("alpha.example")); QVERIFY(csvText.contains("alice")); QVERIFY(!csvText.contains("pa55")); QVERIFY(!csvText.contains("CORP"));
        // The exported CSV imports back.
        QString error; QVERIFY2(ConnectionImport::fromFile(csvPath, csvText.toUtf8(), &error), qPrintable(error));
    }
    void externalToolsExpandSaveAndLaunch() {
        const QJsonObject profile{{"name", "Web"}, {"kind", "connection"}, {"effective", QJsonObject{
            {"host", "web.example"}, {"port", 2222}, {"username", "deploy"}, {"protocol", "ssh"}, {"domain", ""}}}};
        QCOMPARE(ExternalTools::expand("ssh -p %PORT% %username%@%HOSTNAME% # %name% %PROTOCOL%", profile),
                 QString("ssh -p 2222 deploy@web.example # Web SSH"));
        QTemporaryDir dir(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/tools-XXXXXX"); QVERIFY(dir.isValid());
        const auto path = dir.filePath("external-tools.json");
        QVERIFY(ExternalTools::save(path, {{"Ping", "ping", "-c 1 %HOSTNAME%", {}, true}}));
        const auto loaded = ExternalTools::load(path); QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded.first().arguments, QString("-c 1 %HOSTNAME%")); QVERIFY(loaded.first().toolbar);
        QString error; QVERIFY(!ExternalTools::launch({"Empty", {}, {}, {}, false}, profile, &error)); QVERIFY(!error.isEmpty());
#ifndef Q_OS_WIN
        const auto output = dir.filePath("launched.txt");
        QVERIFY2(ExternalTools::launch({"Record", "/bin/sh", "-c \"echo %HOSTNAME%:%PORT% > '" + output + "'\"", dir.path(), false}, profile, &error), qPrintable(error));
        QTRY_VERIFY(QFile::exists(output));
        const auto content = [&] { QFile result(output); return result.open(QIODevice::ReadOnly) ? result.readAll().trimmed() : QByteArray(); };
        QTRY_COMPARE(content(), QByteArray("web.example:2222"));
#endif
    }
};

QTEST_MAIN(ImportExportTest)
#include "import_export.moc"
