#include "AutoUpdate.h"
#include "GridDriver.h"
#include "MainWindow.h"
#include <QAbstractButton>
#include <QAction>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#define WAIT_STORE(window) do { QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open); QVERIFY(waitGridSaved(window)); } while (false)

namespace {
const QString Newer = "999.0.0";
const QString Downloads = "https://github.com/Crayjones/CMRemote/releases/download/v999.0.0/";
QString packageName(const QString& version = Newer) { return "CMRemote-" + version + "-linux-x86_64.appimage"; }
// The smallest file the AppImage helper accepts: ELF magic with the type-2 AppImage marker.
QByteArray appImage(const QByteArray& payload) { return QByteArray("\x7f" "ELF" "\x02\x01\x01\x00" "AI\x02", 11) + payload; }
QString sha(const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }

// Serves GitHub download URLs from local files; the release JSON itself is a file URL.
class FixtureNetwork final : public QNetworkAccessManager {
public:
    QHash<QString, QUrl> routes;
protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request, QIODevice* data) override {
        QNetworkRequest routed(request);
        if (const auto found = routes.constFind(request.url().toString()); found != routes.constEnd()) routed.setUrl(*found);
        return QNetworkAccessManager::createRequest(operation, routed, data);
    }
};
// An HTTP server that sends headers and a few bytes, then stalls: a download stays in flight.
class StallingServer final : public QTcpServer {
public:
    StallingServer() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                    socket->readAll();
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: 1000000\r\n\r\n0123456789"); socket->flush();
                });
            }
        });
        listen(QHostAddress::LocalHost);
    }
    QUrl url() const { return QUrl("http://127.0.0.1:" + QString::number(serverPort()) + "/package"); }
};

struct Published { QString name; QByteArray bytes; bool digest = true; qint64 size = -1; QString digestOverride; };

struct Fixture {
    QTemporaryDir dir;
    FixtureNetwork network;
    QString data() const { return dir.filePath("data"); }
    QString updates() const { return dir.filePath("data/updates"); }
    QString target() const { return dir.filePath("apps/CMRemote.AppImage"); }
    QString sentinel() const { return dir.filePath("apps/notes.txt"); }
    Fixture() {
        QDir().mkpath(dir.filePath("apps")); QDir().mkpath(dir.filePath("served"));
        write(target(), appImage("old version")); QFile::setPermissions(target(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup);
        write(sentinel(), "unrelated");
    }
    Updates::Installation installation() const { return {Updates::Installation::AppImage, target(), {}, "x86_64", {}}; }
    // A "latest release" description whose assets are served from local files.
    QUrl publish(const QString& version, const QList<Published>& files) {
        QJsonArray assets;
        for (const auto& file : files) {
            const auto served = dir.filePath("served/" + file.name);
            write(served, file.bytes);
            const auto url = "https://github.com/Crayjones/CMRemote/releases/download/v" + version + "/" + file.name;
            network.routes.insert(url, QUrl::fromLocalFile(served));
            QJsonObject asset{{"name", file.name}, {"browser_download_url", url}, {"size", file.size >= 0 ? file.size : qint64(file.bytes.size())}};
            if (file.digest) asset.insert("digest", "sha256:" + (file.digestOverride.isEmpty() ? sha(file.bytes) : file.digestOverride));
            assets.append(asset);
        }
        const QJsonObject release{{"tag_name", "v" + version}, {"html_url", "https://github.com/Crayjones/CMRemote/releases/tag/v" + version}, {"assets", assets}};
        const auto path = dir.filePath("release-" + version + ".json");
        write(path, QJsonDocument(release).toJson());
        return QUrl::fromLocalFile(path);
    }
    AutoUpdater::Options options(const QUrl& source, std::optional<Updates::Installation> kind = std::nullopt) {
        AutoUpdater::Options value;
        value.dataDirectory = data(); value.source = source; value.network = &network;
        value.installation = kind ? *kind : installation(); value.firstCheckMs = 0; value.helperTimeoutMs = 5000;
        return value;
    }
};
void clickButton(QMessageBox* box, const QString& text) {
    for (auto* button : box->buttons()) if (button->text() == text) { button->click(); return; }
    QFAIL(qPrintable("No button " + text));
}
bool logged(const QSignalSpy& spy, const QString& text) {
    for (const auto& entry : spy) if (entry.first().toString().contains(text)) return true;
    return false;
}
QStringList entries(const QString& path) { return QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System); }
QJsonObject record(const QString& path) { return QJsonDocument::fromJson(read(path)).object(); }
// A stand-in for the CMRemote process the helper waits for.
struct Parent {
    QProcess process;
    Parent() { process.start("/bin/sleep", {"60"}); process.waitForStarted(); }
    ~Parent() { process.kill(); process.waitForFinished(); }
    qint64 pid() const { return process.processId(); }
    void exit() { process.kill(); process.waitForFinished(); }
};
}

class AutoUpdateTest : public QObject {
    Q_OBJECT
private slots:
    void packagesMatchInstallationKindAndArchitecture() {
        const QList<Updates::Asset> assets{{"CMRemote-1.0.0-macos-arm64.dmg", "u", {}, 10}, {"CMRemote-1.0.0-macos-x64.dmg", "u", {}, 10},
            {"CMRemote-1.0.0-linux-x86_64.appimage", "u", {}, 10}, {"CMRemote-1.0.0-linux-x86_64.flatpak", "u", {}, 10},
            {"CMRemote-1.0.0-windows-x64-setup.exe", "u", {}, 10}};
        Updates::Installation mac{Updates::Installation::MacBundle, "/Applications/CMRemote.app", "org.remotemanager.RemoteManager", "x64", {}};
        QCOMPARE(Updates::installerFor(assets, "1.0.0", mac)->name, QString("CMRemote-1.0.0-macos-x64.dmg"));
        mac.architecture = "arm64"; QCOMPARE(Updates::installerFor(assets, "1.0.0", mac)->name, QString("CMRemote-1.0.0-macos-arm64.dmg"));
        const Updates::Installation image{Updates::Installation::AppImage, "/tmp/CMRemote.AppImage", {}, "x86_64", {}};
        QCOMPARE(Updates::installerFor(assets, "1.0.0", image)->name, QString("CMRemote-1.0.0-linux-x86_64.appimage"));
        QVERIFY(!Updates::installerFor(assets, "1.0.1", image));
        QVERIFY(!Updates::installerFor(assets, "1.0.0", Updates::Installation{}));
        // Larger than any installer CMRemote accepts.
        QVERIFY(!Updates::installerFor({{"CMRemote-1.0.0-linux-x86_64.appimage", "u", {}, 1ll << 40}}, "1.0.0", image));
        // A development build is never a release package.
        const auto here = Updates::detectInstallation();
        QVERIFY(!here.supported()); QVERIFY(!here.reason.isEmpty());
    }
    void checksumsRejectMissingAndConflictingEntries() {
        const auto a = QString(64, 'a'), b = QString(64, 'b');
        QCOMPARE(Updates::checksumFor((a + "  x.dmg\n" + b + "  y.dmg\n").toLatin1(), "x.dmg"), a);
        QCOMPARE(Updates::checksumFor((a + " *x.dmg\n" + a + "  x.dmg\n").toLatin1(), "x.dmg"), a);
        QVERIFY(Updates::checksumFor((a + "  x.dmg\n" + b + "  x.dmg\n").toLatin1(), "x.dmg").isEmpty());
        QVERIFY(Updates::checksumFor((a + "  x.dmg.zip\n").toLatin1(), "x.dmg").isEmpty());
        // Size comes from the release description.
        const auto release = Updates::parseLatestRelease(QString("{\"tag_name\":\"v1.0.0\",\"html_url\":\"https://github.com/Crayjones/CMRemote/releases/tag/v1.0.0\",\"assets\":["
            "{\"name\":\"a.dmg\",\"size\":1234,\"browser_download_url\":\"https://github.com/Crayjones/CMRemote/releases/download/v1.0.0/a.dmg\"}]}").toUtf8());
        QVERIFY(release); QCOMPARE(release->assets.first().size, qint64(1234));
    }
    void newerReleaseDownloadsVerifiesAndRecordsIt() {
        Fixture f; const auto bytes = appImage("new version");
        AutoUpdater updater(f.options(f.publish(Newer, {{packageName(), bytes}})));
        QSignalSpy ready(&updater, &AutoUpdater::readyToInstall), log(&updater, &AutoUpdater::log);
        updater.setAutomatic(true);
        QVERIFY(ready.wait(5000)); QCOMPARE(ready.first().first().toString(), Newer);
        QVERIFY(updater.offered()); QVERIFY(!updater.busy());
        // Downloaded is not consent: nothing installs until the user accepts.
        QVERIFY(!updater.installOnExit()); updater.prepare();
        QSignalSpy prepared(&updater, &AutoUpdater::preparationFinished); QVERIFY(prepared.wait(2000)); QVERIFY(!prepared.first().first().toBool());
        updater.accept(); QVERIFY(updater.installOnExit()); updater.decline(); QVERIFY(!updater.installOnExit());
        QCOMPARE(read(f.updates() + "/" + packageName()), bytes);
        const auto pending = record(f.updates() + "/pending.json");
        QCOMPARE(pending.value("version").toString(), Newer); QCOMPARE(pending.value("sha256").toString(), sha(bytes));
        QCOMPARE(pending.value("size").toInteger(), qint64(bytes.size()));
        QCOMPARE(QFileInfo(f.updates()).permissions() & (QFile::ReadGroup | QFile::ReadOther | QFile::WriteOther), QFileDevice::Permissions());
        QVERIFY(!QFile::exists(f.updates() + "/" + packageName() + ".part"));
        QCOMPARE(read(f.target()), appImage("old version"));
    }
    void currentAndUnsupportedReleasesAreNotDownloaded() {
        Fixture f;
        {
            AutoUpdater updater(f.options(f.publish(Updates::currentVersion(), {{"CMRemote-" + Updates::currentVersion() + "-linux-x86_64.appimage", appImage("x")}})));
            QSignalSpy checked(&updater, &AutoUpdater::checked); updater.checkNow(true);
            QVERIFY(checked.wait(5000)); QVERIFY(checked.first().at(0).toBool()); QVERIFY(!checked.first().at(3).toBool());
            QVERIFY(!updater.ready());
        }
        {
            AutoUpdater updater(f.options(f.publish(Newer, {{packageName(), appImage("x")}}), Updates::Installation{Updates::Installation::None, {}, {}, {}, "Portable copies update by download."}));
            QSignalSpy checked(&updater, &AutoUpdater::checked), log(&updater, &AutoUpdater::log); updater.checkNow(true);
            QVERIFY(checked.wait(5000)); QVERIFY(!checked.first().at(3).toBool());
            QTRY_VERIFY(logged(log, "Portable copies update by download."));
            QVERIFY(!updater.ready()); QVERIFY(!QFile::exists(f.updates() + "/" + packageName()));
        }
    }
    void checksumListIsUsedWhenNoDigestIsPublished() {
        Fixture f; const auto bytes = appImage("from sums");
        const auto sums = (sha(bytes) + "  " + packageName() + "\n").toLatin1();
        AutoUpdater updater(f.options(f.publish(Newer, {{packageName(), bytes, false}, {"SHA256SUMS.txt", sums, false}})));
        QSignalSpy ready(&updater, &AutoUpdater::readyToInstall); updater.checkNow(true);
        QVERIFY(ready.wait(5000));
        Fixture missing;
        AutoUpdater without(missing.options(missing.publish(Newer, {{packageName(), bytes, false}})));
        QSignalSpy log(&without, &AutoUpdater::log); without.checkNow(true);
        QTRY_VERIFY(logged(log, "has no published SHA-256")); QVERIFY(!without.ready());
        Fixture conflicting;
        const auto both = sums + (QString(64, 'c') + "  " + packageName() + "\n").toLatin1();
        AutoUpdater ambiguous(conflicting.options(conflicting.publish(Newer, {{packageName(), bytes, false}, {"SHA256SUMS.txt", both, false}})));
        QSignalSpy conflict(&ambiguous, &AutoUpdater::log); ambiguous.checkNow(true);
        QTRY_VERIFY(logged(conflict, "has no single checksum")); QVERIFY(!ambiguous.ready());
        QVERIFY(!QFile::exists(conflicting.updates() + "/" + packageName()));
    }
    void corruptOversizeAndIncompleteDownloadsAreDeleted() {
        const auto bytes = appImage("payload");
        const QList<std::pair<Published, QString>> cases{
            {{packageName(), bytes, true, -1, QString(64, '0')}, "failed verification"},
            {{packageName(), bytes, true, bytes.size() - 3}, "larger than published"},
            {{packageName(), bytes, true, bytes.size() + 3}, "incomplete"}};
        for (const auto& [published, message] : cases) {
            Fixture f;
            AutoUpdater updater(f.options(f.publish(Newer, {published})));
            QSignalSpy log(&updater, &AutoUpdater::log); updater.checkNow(true);
            QTRY_VERIFY2(logged(log, message), qPrintable(message));
            QTRY_VERIFY(!updater.busy());
            QVERIFY(!updater.ready());
            QCOMPARE(entries(f.updates()).filter("CMRemote-"), QStringList());
            QVERIFY(!QFile::exists(f.updates() + "/pending.json"));
        }
    }
    void manualUpdateInstallsEvenWhenAutomaticUpdatesAreOff() {
        Fixture f;
        AutoUpdater updater(f.options(f.publish(Newer, {{packageName(), appImage("manual")}})));
        QSignalSpy ready(&updater, &AutoUpdater::readyToInstall), checked(&updater, &AutoUpdater::checked);
        updater.setAutomatic(false);
        updater.checkNow(false); QTest::qWait(100); QVERIFY(!updater.busy()); QVERIFY(ready.isEmpty());
        updater.checkNow(true);
        QVERIFY(ready.wait(5000)); QCOMPARE(checked.size(), 1); QVERIFY(checked.first().at(3).toBool());
        QVERIFY(updater.offered()); updater.accept(); QVERIFY(updater.installOnExit());
        // A later automatic opt-out does not take back what the user asked for.
        updater.setAutomatic(true); updater.setAutomatic(false); QVERIFY(updater.installOnExit());
    }
    void disablingStopsAutomaticDownloadsButNotRequestedOnes() {
        StallingServer server; QVERIFY(server.isListening());
        for (const bool manual : {false, true}) {
            Fixture f;
            const auto source = f.publish(Newer, {{packageName(), appImage("slow")}});
            f.network.routes.insert(Downloads + packageName(), server.url());
            AutoUpdater updater(f.options(source));
            updater.setAutomatic(true);
            if (manual) updater.checkNow(true);
            QTRY_VERIFY(QFile::exists(f.updates() + "/" + packageName() + ".part"));
            updater.setAutomatic(false);
            if (manual) { QVERIFY(updater.busy()); continue; }
            QVERIFY(!updater.busy());
            QVERIFY(!QFile::exists(f.updates() + "/" + packageName() + ".part"));
            QTest::qWait(100); QVERIFY(!updater.ready());
        }
    }
    void pendingUpdateIsRecoveredWithoutNetworkAndTamperingDiscardsIt() {
        Fixture f; const auto bytes = appImage("recover");
        { AutoUpdater first(f.options(f.publish(Newer, {{packageName(), bytes}}))); QSignalSpy ready(&first, &AutoUpdater::readyToInstall); first.checkNow(true); QVERIFY(ready.wait(5000)); }
        const auto offline = QUrl::fromLocalFile(f.dir.filePath("missing.json"));
        {
            AutoUpdater again(f.options(offline)); QSignalSpy ready(&again, &AutoUpdater::readyToInstall);
            again.setAutomatic(true);
            QVERIFY(ready.wait(5000)); QCOMPARE(again.readyVersion(), Newer); QVERIFY(again.offered()); QVERIFY(!again.installOnExit());
        }
        write(f.updates() + "/" + packageName(), appImage("tampered"));
        {
            AutoUpdater tampered(f.options(offline)); QSignalSpy log(&tampered, &AutoUpdater::log);
            QTRY_VERIFY(logged(log, "changed on disk")); QVERIFY(!tampered.ready());
            QVERIFY(!QFile::exists(f.updates() + "/" + packageName())); QVERIFY(!QFile::exists(f.updates() + "/pending.json"));
        }
    }
    void secondInstanceNeverManagesUpdates() {
        Fixture f;
        AutoUpdater first(f.options(f.publish(Newer, {{packageName(), appImage("one")}})));
        QSignalSpy ready(&first, &AutoUpdater::readyToInstall); first.checkNow(true); QVERIFY(ready.wait(5000));
        AutoUpdater second(f.options(f.publish(Newer, {{packageName(), appImage("one")}})));
        QSignalSpy log(&second, &AutoUpdater::log), checked(&second, &AutoUpdater::checked);
        second.checkNow(true);
        QVERIFY(checked.wait(5000)); QVERIFY(!checked.first().at(3).toBool());
        QVERIFY(logged(log, "another CMRemote window manages updates")); QVERIFY(!second.offered());
    }
    void committedHelperReplacesOnlyTheTargetAfterExit() {
        Fixture f; const auto bytes = appImage("installed");
        Parent parent;
        auto options = f.options(f.publish(Newer, {{packageName(), bytes}})); options.parentPid = parent.pid();
        AutoUpdater updater(options);
        QSignalSpy ready(&updater, &AutoUpdater::readyToInstall), prepared(&updater, &AutoUpdater::preparationFinished);
        updater.checkNow(true); QVERIFY(ready.wait(5000));
        QVERIFY(!updater.commit(false)); // nothing prepared yet
        updater.accept(); updater.prepare(); QVERIFY(prepared.wait(5000)); QVERIFY2(prepared.first().at(0).toBool(), qPrintable(prepared.first().at(1).toString()));
        QVERIFY(updater.prepared());
        // The helper is waiting: nothing changes while CMRemote still runs.
        QTest::qWait(300); QCOMPARE(read(f.target()), appImage("old version"));
        QVERIFY(updater.commit(false));
        parent.exit();
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(f.updates() + "/result.json"), 10000);
        const auto result = record(f.updates() + "/result.json");
        QCOMPARE(result.value("status").toString(), QString("installed")); QCOMPARE(result.value("version").toString(), Newer);
        QCOMPARE(read(f.target()), bytes);
        QVERIFY(QFileInfo(f.target()).permissions() & QFile::ExeGroup); QVERIFY(!(QFileInfo(f.target()).permissions() & QFile::WriteGroup));
        QCOMPARE(read(f.sentinel()), QByteArray("unrelated"));
        QCOMPARE(entries(f.dir.filePath("apps")), QStringList({"CMRemote.AppImage", "notes.txt"}));
        QVERIFY(!QFile::exists(f.updates() + "/" + packageName())); QVERIFY(!QFile::exists(f.updates() + "/pending.json"));
        QVERIFY(!QFile::exists(f.updates() + "/install.lock"));
        QCOMPARE(entries(f.updates()).filter("session-"), QStringList());
    }
    void uncommittedHelperExitsWithoutInstalling() {
        Fixture f;
        Parent parent;
        auto options = f.options(f.publish(Newer, {{packageName(), appImage("never")}})); options.parentPid = parent.pid();
        AutoUpdater updater(options);
        QSignalSpy ready(&updater, &AutoUpdater::readyToInstall), prepared(&updater, &AutoUpdater::preparationFinished);
        updater.checkNow(true); QVERIFY(ready.wait(5000));
        updater.accept(); updater.prepare(); QVERIFY(prepared.wait(5000)); QVERIFY(prepared.first().at(0).toBool());
        // CMRemote "crashes" or the quit is canceled and it later exits without committing.
        parent.exit();
        QTRY_COMPARE_WITH_TIMEOUT(entries(f.updates()).filter("session-"), QStringList(), 10000);
        QCOMPARE(read(f.target()), appImage("old version"));
        QVERIFY(!QFile::exists(f.updates() + "/result.json"));
        QVERIFY(QFile::exists(f.updates() + "/" + packageName())); // still pending for the next quit
    }
    void helperFailureKeepsTheOldVersionAndIsReportedOnce() {
        Fixture f;
        Parent parent;
        const auto source = f.publish(Newer, {{packageName(), appImage("broken")}});
        auto options = f.options(source); options.parentPid = parent.pid();
        {
            AutoUpdater updater(options);
            QSignalSpy ready(&updater, &AutoUpdater::readyToInstall), prepared(&updater, &AutoUpdater::preparationFinished);
            updater.checkNow(true); QVERIFY(ready.wait(5000));
            updater.accept(); updater.prepare(); QVERIFY(prepared.wait(5000)); QVERIFY(prepared.first().at(0).toBool());
            // Changed after the last check: the helper rehashes after CMRemote exits.
            write(f.updates() + "/" + packageName(), appImage("swapped"));
            QVERIFY(updater.commit(true));
            parent.exit();
            QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(f.updates() + "/result.json"), 10000);
        }
        QCOMPARE(record(f.updates() + "/result.json").value("status").toString(), QString("failed"));
        QCOMPARE(read(f.target()), appImage("old version"));
        QCOMPARE(read(f.sentinel()), QByteArray("unrelated"));
        {
            AutoUpdater next(f.options(source)); QSignalSpy log(&next, &AutoUpdater::log);
            QTRY_VERIFY(logged(log, "installing CMRemote " + Newer + " failed"));
            QVERIFY(!QFile::exists(f.updates() + "/result.json"));
            // Not retried automatically.
            next.setAutomatic(true);
            QTRY_VERIFY(logged(log, "not installed again automatically")); QVERIFY(!next.ready());
        }
        {
            AutoUpdater manual(f.options(source)); QSignalSpy log(&manual, &AutoUpdater::log);
            QTRY_VERIFY(!logged(log, "failed")); // reported once
            QSignalSpy ready(&manual, &AutoUpdater::readyToInstall);
            manual.checkNow(true);
            QVERIFY(ready.wait(5000)); // an explicit check retries
        }
    }
    void readyPromptInstallsOnlyWhenTheUserChooses() {
        Fixture f; const auto bytes = appImage("from window");
        Parent parent;
        auto options = f.options(f.publish(Newer, {{packageName(), bytes}})); options.parentPid = parent.pid();
        MainWindow w(f.dir.filePath("confCons.xml"), true); w.show(); WAIT_STORE(w);
        auto* updater = new AutoUpdater(options); w.attachUpdater(updater);
        updater->checkNow(true);
        QMessageBox* prompt = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((prompt = w.findChild<QMessageBox*>("updateReady")), 5000);
        QCOMPARE(prompt->defaultButton()->text(), QString("Not Now"));
        QCOMPARE(prompt->escapeButton(), prompt->defaultButton());
        QStringList labels; for (auto* button : prompt->buttons()) labels << button->text();
        QCOMPARE(labels.size(), 4); QVERIFY(labels.contains("Install When I Quit")); QVERIFY(labels.contains("Skip This Version"));
        clickButton(prompt, "Install When I Quit");
        QVERIFY(w.isVisible()); QVERIFY(updater->installOnExit());
        w.close();
        QTRY_VERIFY_WITH_TIMEOUT(!w.isVisible(), 10000);
        QVERIFY(updater->prepared());
        QStringList sessions = entries(f.updates()).filter("session-"); QCOMPARE(sessions.size(), 1);
        QCOMPARE(read(f.updates() + "/" + sessions.first() + "/commit"), QByteArray("0"));
        parent.exit();
        QTRY_COMPARE_WITH_TIMEOUT(read(f.target()), bytes, 10000);
    }
    void failedPreparationKeepsTheWindowUntilTheUserQuitsWithoutUpdating() {
        Fixture f;
        Parent parent;
        auto options = f.options(f.publish(Newer, {{packageName(), appImage("prepare")}})); options.parentPid = parent.pid();
        MainWindow w(f.dir.filePath("confCons.xml"), true); w.show(); WAIT_STORE(w);
        auto* updater = new AutoUpdater(options); w.attachUpdater(updater);
        updater->checkNow(true);
        QMessageBox* prompt = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((prompt = w.findChild<QMessageBox*>("updateReady")), 5000);
        clickButton(prompt, "Install When I Quit");
        write(f.updates() + "/" + packageName(), appImage("tampered"));
        w.close();
        QMessageBox* failed = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((failed = w.findChild<QMessageBox*>("updatePrepareFailed")), 5000);
        QVERIFY(w.isVisible()); QVERIFY(!updater->prepared());
        QCOMPARE(failed->defaultButton(), failed->button(QMessageBox::Cancel));
        clickButton(failed, "Quit Without Updating");
        QTRY_VERIFY_WITH_TIMEOUT(!w.isVisible(), 10000);
        QCOMPARE(entries(f.updates()).filter("session-"), QStringList());
        QCOMPARE(read(f.target()), appImage("old version"));
    }
    void notNowAndDismissingInstallNothingOnQuit() {
        Fixture f;
        Parent parent;
        auto options = f.options(f.publish(Newer, {{packageName(), appImage("declined")}})); options.parentPid = parent.pid();
        MainWindow w(f.dir.filePath("confCons.xml"), true); w.show(); WAIT_STORE(w);
        auto* updater = new AutoUpdater(options); w.attachUpdater(updater);
        updater->checkNow(true);
        QMessageBox* prompt = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((prompt = w.findChild<QMessageBox*>("updateReady")), 5000);
        // Changing one's mind: accept, then Check for Updates offers it again and Not Now withdraws.
        clickButton(prompt, "Install When I Quit"); QVERIFY(updater->installOnExit());
        w.findChild<QAction*>("actionCheckUpdates")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT((prompt = w.findChild<QMessageBox*>("updateReady")) && prompt->isVisible(), 5000);
        QTest::keyClick(prompt, Qt::Key_Escape); // animated click on macOS
        QTRY_VERIFY(!updater->installOnExit());
        w.close();
        QTRY_VERIFY_WITH_TIMEOUT(!w.isVisible(), 10000);
        QVERIFY(!updater->prepared()); QCOMPARE(entries(f.updates()).filter("session-"), QStringList());
        QCOMPARE(read(f.target()), appImage("old version"));
        QVERIFY(QFile::exists(f.updates() + "/pending.json")); // offered again next start
    }
    void skippedVersionIsNotOfferedAgainUntilTheUserChecks() {
        Fixture f;
        const auto source = f.publish(Newer, {{packageName(), appImage("skipped")}});
        {
            MainWindow w(f.dir.filePath("confCons.xml"), true); w.show(); WAIT_STORE(w);
            auto* updater = new AutoUpdater(f.options(source)); w.attachUpdater(updater);
            updater->checkNow(true);
            QMessageBox* prompt = nullptr;
            QTRY_VERIFY_WITH_TIMEOUT((prompt = w.findChild<QMessageBox*>("updateReady")), 5000);
            clickButton(prompt, "Skip This Version");
            QVERIFY(!updater->ready()); QVERIFY(!QFile::exists(f.updates() + "/" + packageName()));
            QCOMPARE(record(f.updates() + "/skipped.json").value("version").toString(), Newer);
        }
        {
            AutoUpdater automatic(f.options(source)); QSignalSpy log(&automatic, &AutoUpdater::log);
            automatic.setAutomatic(true);
            QTRY_VERIFY(logged(log, "you chose to skip it")); QVERIFY(!automatic.ready()); QVERIFY(!automatic.busy());
        }
        AutoUpdater manual(f.options(source)); QSignalSpy ready(&manual, &AutoUpdater::readyToInstall);
        manual.checkNow(true); QVERIFY(ready.wait(5000)); QVERIFY(manual.offered());
        QVERIFY(!QFile::exists(f.updates() + "/skipped.json"));
    }
    void checkForUpdatesShowsOneAnswerFromTheUpdater() {
        Fixture f;
        MainWindow w(f.dir.filePath("confCons.xml"), true); w.show(); WAIT_STORE(w);
        auto* updater = new AutoUpdater(f.options(f.publish(Newer, {{packageName(), appImage("menu")}}))); w.attachUpdater(updater);
        auto* action = w.findChild<QAction*>("actionCheckUpdates");
        action->trigger(); action->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(w.findChild<QMessageBox*>("updateReady"), 5000);
        const auto dialogs = w.findChildren<QMessageBox*>("updateDialog");
        QCOMPARE(dialogs.size(), 1);
        QVERIFY(dialogs.first()->text().contains("It is downloading now. When it is ready, CMRemote asks whether to install it."));
        // Nothing left queued: a later automatic check opens no dialog.
        dialogs.first()->close(); delete dialogs.first();
        updater->setAutomatic(true); QTest::qWait(300);
        QCOMPARE(w.findChildren<QMessageBox*>("updateDialog").size(), 0);
    }
    void checkDuringSavedUpdateVerificationIsAnswered() {
        Fixture f;
        const auto source = f.publish(Newer, {{packageName(), appImage("saved")}});
        { AutoUpdater first(f.options(source)); QSignalSpy ready(&first, &AutoUpdater::readyToInstall); first.checkNow(true); QVERIFY(ready.wait(5000)); }
        AutoUpdater again(f.options(source)); QSignalSpy checked(&again, &AutoUpdater::checked);
        again.checkNow(true); // recovery is still verifying the saved package
        QVERIFY(checked.wait(5000)); QVERIFY(checked.first().at(0).toBool()); QVERIFY(checked.first().at(3).toBool());
        QVERIFY(again.offered());
    }
};

QTEST_MAIN(AutoUpdateTest)
#include "auto_update.moc"
