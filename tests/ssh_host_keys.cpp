#include "SshHostKeyStore.h"
#include "SshHostKeyService.h"
#include "SshHostKeyDialog.h"
#include "DataPaths.h"
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QScopeGuard>
#include <QElapsedTimer>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QTest>

namespace {
QString fixtureDirectory() {
    const auto build = qEnvironmentVariable("RM_BUILD_DIR");
    if (build.isEmpty()) qFatal("Run SSH trust tests through scripts/env.py.");
    return build + "/test-data/ssh-host-keys-XXXXXX";
}
SshHostKey key(char seed = 7, QString host = "OWNED.example", quint16 port = 22) {
    auto bytes = QByteArray::fromHex("0000000b7373682d6564323535313900000020"); bytes += QByteArray(32, seed);
    return makeSshHostKey(host, port, bytes);
}
QByteArray contents(const QString& path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
void write(const QString& path, const QByteArray& bytes) { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); }
}
class SshHostKeysTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(QDir().mkpath(qEnvironmentVariable("RM_BUILD_DIR") + "/test-data")); }
    void hostTrustPathIsSeparateFromConnectionFiles() {
        const auto directory = applicationDataDirectory(QString::fromUtf8(QT_TESTCASE_SOURCEDIR));
        QCOMPARE(applicationSshHostKeyPath(directory), QDir(directory).filePath("ssh-host-keys.json"));
        QVERIFY(applicationSshHostKeyPath(directory) != applicationConnectionPath(directory));
#ifdef Q_OS_MACOS
        QVERIFY(directory.endsWith("/.dev/macos/data"));
#else
        QVERIFY(directory.endsWith("/.dev/linux/data"));
#endif
        QTemporaryDir fixture(fixtureDirectory()); const auto path = applicationSshHostKeyPath(fixture.path());
        SshHostKeyStore store(path); const auto checked = store.inspect(key());
        QVERIFY(store.decide(checked.inspection->ticket, SshHostKeyDecision::TrustAndSave).allowed);
        QVERIFY(QFile::exists(path)); QVERIFY(!QFile::exists(applicationConnectionPath(fixture.path())));
    }
    void unknownHostKeyChoicesPrecedeAuthentication() {
        QTemporaryDir directory(fixtureDirectory()); QVERIFY(directory.isValid()); const auto path = directory.filePath("ssh-host-keys.json");
        SshHostKeyStore store(path); const auto check = store.inspect(key());
        QVERIFY(check.result.ok); QVERIFY(check.inspection); QVERIFY(!check.allowed);
        QCOMPARE(check.inspection->status, SshHostKeyStatus::Unknown);
        QCOMPARE(check.inspection->candidate.host, QString("owned.example"));
        QVERIFY(!store.decide(check.inspection->ticket, SshHostKeyDecision::UseTrusted).allowed);
        const auto canceled = store.decide(check.inspection->ticket, SshHostKeyDecision::Cancel);
        QVERIFY(canceled.canceled); QVERIFY(!canceled.allowed); QVERIFY(!QFile::exists(path));
        const auto again = store.inspect(key()); const auto saved = store.decide(again.inspection->ticket, SshHostKeyDecision::TrustAndSave);
        QVERIFY(saved.result.ok); QVERIFY(saved.allowed); QVERIFY(saved.committed);
        SshHostKeyStore reopened(path); const auto known = reopened.inspect(key());
        QCOMPARE(known.inspection->status, SshHostKeyStatus::Trusted); QVERIFY(!known.allowed);
        QVERIFY(reopened.decide(known.inspection->ticket, SshHostKeyDecision::UseTrusted).allowed);
#ifdef Q_OS_UNIX
        const auto permissions = QFile::permissions(path);
        QVERIFY(!(permissions & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther)));
#endif
    }
    void changedHostKeyChoicesWarnAndRequireExplicitReplacement() {
        QTemporaryDir directory(fixtureDirectory()); const auto path = directory.filePath("ssh-host-keys.json"); SshHostKeyStore store(path);
        const auto first = store.inspect(key()); QVERIFY(store.decide(first.inspection->ticket, SshHostKeyDecision::TrustAndSave).allowed);
        const auto original = contents(path); const auto changed = store.inspect(key(8));
        QCOMPARE(changed.inspection->status, SshHostKeyStatus::Changed); QVERIFY(changed.inspection->previous);
        QCOMPARE(changed.inspection->previous->fingerprint, key().fingerprint);
        QVERIFY(changed.inspection->candidate.fingerprint != changed.inspection->previous->fingerprint);
        QVERIFY(!store.decide(changed.inspection->ticket, SshHostKeyDecision::TrustAndSave).allowed); QCOMPARE(contents(path), original);
        QVERIFY(store.decide(changed.inspection->ticket, SshHostKeyDecision::ConnectOnce).allowed); QCOMPARE(contents(path), original);
        const auto replacement = store.inspect(key(8)); const auto saved = store.decide(replacement.inspection->ticket, SshHostKeyDecision::ReplaceAndConnect);
        QVERIFY(saved.allowed); QVERIFY(saved.committed); QVERIFY(contents(path) != original);
        QCOMPARE(store.inspect(key(8)).inspection->status, SshHostKeyStatus::Trusted);
        QCOMPARE(store.inspect(key()).inspection->status, SshHostKeyStatus::Changed);
    }
    void connectOnceNeverMutatesTrustCache() {
        QTemporaryDir directory(fixtureDirectory()); const auto path = directory.filePath("ssh-host-keys.json"); SshHostKeyStore store(path);
        const auto first = store.inspect(key()); QVERIFY(store.decide(first.inspection->ticket, SshHostKeyDecision::ConnectOnce).allowed); QVERIFY(!QFile::exists(path));
        const auto saved = store.inspect(key()); QVERIFY(store.decide(saved.inspection->ticket, SshHostKeyDecision::TrustAndSave).allowed);
        const auto original = contents(path); const auto separatePort = store.inspect(key(7, "owned.example", 2222));
        QCOMPARE(separatePort.inspection->status, SshHostKeyStatus::Unknown);
        const auto once = store.decide(separatePort.inspection->ticket, SshHostKeyDecision::ConnectOnce);
        QVERIFY(once.allowed); QVERIFY(!once.committed); QCOMPARE(contents(path), original);
        QVERIFY(!store.decide(separatePort.inspection->ticket, SshHostKeyDecision::ConnectOnce).allowed);
    }
    void hostTrustSaveFailureBlocksAuthAndSyncWarningKeepsCommit() {
        QTemporaryDir directory(fixtureDirectory()); const auto path = directory.filePath("ssh-host-keys.json"); SshHostKeyStore store(path);
        store.setFault(AtomicFile::Fault::BeforeReplace); const auto first = store.inspect(key());
        const auto failed = store.decide(first.inspection->ticket, SshHostKeyDecision::TrustAndSave);
        QVERIFY(!failed.result.ok); QVERIFY(!failed.allowed); QVERIFY(!failed.committed); QVERIFY(!QFile::exists(path));
        store.setFault(AtomicFile::Fault::DirectorySync);
        const auto saved = store.decide(first.inspection->ticket, SshHostKeyDecision::TrustAndSave);
        QVERIFY(saved.result.ok); QVERIFY(saved.committed); QVERIFY(!saved.allowed); QVERIFY(!saved.result.warning.isEmpty());
        const auto committed = contents(path); QVERIFY(!committed.isEmpty());
        QVERIFY(!store.decide(first.inspection->ticket, SshHostKeyDecision::ConnectOnce).allowed);
        const auto acknowledged = store.decide(first.inspection->ticket, SshHostKeyDecision::AcknowledgeWarning);
        QVERIFY(acknowledged.allowed); QVERIFY(acknowledged.committed); QCOMPARE(contents(path), committed);
        const auto changed = store.inspect(key(8)); const auto changedSave = store.decide(changed.inspection->ticket, SshHostKeyDecision::ReplaceAndConnect);
        QVERIFY(changedSave.committed); QVERIFY(!changedSave.allowed); const auto replaced = contents(path);
        const auto canceled = store.decide(changed.inspection->ticket, SshHostKeyDecision::Cancel);
        QVERIFY(canceled.canceled); QVERIFY(canceled.committed); QVERIFY(!canceled.allowed); QCOMPARE(contents(path), replaced);
    }
    void staleAndConcurrentHostDecisionsCannotAuthorize() {
        QTemporaryDir directory(fixtureDirectory()); const auto path = directory.filePath("ssh-host-keys.json"); SshHostKeyStore a(path), b(path);
        const auto checkA = a.inspect(key()), checkB = b.inspect(key(8));
        QVERIFY(b.decide(checkB.inspection->ticket, SshHostKeyDecision::TrustAndSave).allowed); const auto committed = contents(path);
        const auto stale = a.decide(checkA.inspection->ticket, SshHostKeyDecision::TrustAndSave);
        QVERIFY(stale.conflict); QVERIFY(!stale.allowed); QVERIFY(!stale.committed); QCOMPARE(contents(path), committed);
        QVERIFY(!a.decide(checkA.inspection->ticket, SshHostKeyDecision::ConnectOnce).allowed);
        const auto current = a.inspect(key()); QCOMPARE(current.inspection->status, SshHostKeyStatus::Changed);
        a.cancel(current.inspection->ticket); QVERIFY(!a.decide(current.inspection->ticket, SshHostKeyDecision::ReplaceAndConnect).allowed);
        const auto awaiting = a.inspect(key());
        // AtomicFile also checks immediately at replacement, independently of the prompt snapshot.
        a.setCommitGate([&](const auto& commit) { write(path, committed + " "); return commit(); });
        const auto raced = a.decide(awaiting.inspection->ticket, SshHostKeyDecision::ReplaceAndConnect);
        QVERIFY(!raced.allowed); QVERIFY(!raced.committed); QVERIFY(raced.conflict); QCOMPARE(contents(path), committed + " ");
    }
    void hostKeyPromptsUsePlainTextExplicitChoicesAndDefaultCancel() {
        QTemporaryDir directory(fixtureDirectory()); SshHostKeyStore store(directory.filePath("ssh-host-keys.json")); QWidget owner;
        const auto unknown = store.inspect(key(7, "<owned>")); int replies = 0; auto choice = SshHostKeyDecision::UseTrusted;
        auto* prompt = new SshHostKeyDialog(*unknown.inspection, [&](auto value) { ++replies; choice = value; }, &owner);
        QCOMPARE(prompt->textFormat(), Qt::PlainText); QVERIFY(prompt->text().contains("<owned>"));
        QCOMPARE(prompt->defaultButton()->text(), QString("Cancel")); QCOMPARE(prompt->escapeButton()->text(), QString("Cancel"));
        QCOMPARE(prompt->findChild<QPushButton*>("sshTrustSave")->text(), QString("Trust and save"));
        QCOMPARE(prompt->findChild<QPushButton*>("sshTrustOnce")->text(), QString("Connect once"));
        prompt->reject(); QCOMPARE(replies, 1); QCOMPARE(choice, SshHostKeyDecision::Cancel); prompt->reject(); QCOMPARE(replies, 1);
        QVERIFY(store.decide(unknown.inspection->ticket, SshHostKeyDecision::TrustAndSave).allowed);
        const auto changed = store.inspect(key(8, "<owned>"));
        auto* warning = new SshHostKeyDialog(*changed.inspection, [&](auto value) { choice = value; }, &owner);
        QCOMPARE(warning->icon(), QMessageBox::Warning); QCOMPARE(warning->textFormat(), Qt::PlainText);
        QVERIFY(warning->text().contains(changed.inspection->previous->fingerprint)); QVERIFY(warning->text().contains(changed.inspection->candidate.fingerprint));
        QCOMPARE(warning->defaultButton()->text(), QString("Cancel"));
        QCOMPARE(warning->findChild<QPushButton*>("sshTrustOnce")->text(), QString("Connect once without replacement"));
        auto* replace = warning->findChild<QPushButton*>("sshTrustSave"); QCOMPARE(replace->text(), QString("Replace trusted key and connect"));
        replace->click(); QCOMPARE(choice, SshHostKeyDecision::ReplaceAndConnect);
    }
    void committedSaveWarningPromptRequiresAcknowledgmentOrCancel() {
        QTemporaryDir directory(fixtureDirectory()); const auto path = directory.filePath("ssh-host-keys.json"); SshHostKeyStore store(path); QWidget owner;
        store.setFault(AtomicFile::Fault::DirectorySync); const auto inspected = store.inspect(key());
        const auto saved = store.decide(inspected.inspection->ticket, SshHostKeyDecision::TrustAndSave); QVERIFY(saved.committed); QVERIFY(!saved.allowed);
        const auto committed = contents(path); auto choice = SshHostKeyDecision::UseTrusted;
        auto* prompt = new SshHostKeyDialog(saved, [&](auto value) { choice = value; }, &owner);
        QCOMPARE(prompt->textFormat(), Qt::PlainText); QCOMPARE(prompt->defaultButton()->text(), QString("Cancel"));
        QVERIFY(prompt->text().contains(saved.result.warning)); QVERIFY(!prompt->findChild<QPushButton*>("sshTrustOnce"));
        prompt->findChild<QPushButton*>("sshTrustAcknowledge")->click(); QCOMPARE(choice, SshHostKeyDecision::AcknowledgeWarning);
        QVERIFY(store.decide(inspected.inspection->ticket, choice).allowed); QCOMPARE(contents(path), committed);
    }
    void hostTrustServiceRunsOffGuiAndRejectsStaleGenerations() {
        QTemporaryDir directory(fixtureDirectory()); const auto path = directory.filePath("ssh-host-keys.json");
        const auto gui = QThread::currentThreadId(); SshHostKeyService service(path); SshHostKeyReply inspected; bool done = false;
        const auto request = service.check(key(), 17, [&](SshHostKeyReply reply) { QCOMPARE(QThread::currentThreadId(), gui); inspected = std::move(reply); done = true; });
        QTRY_VERIFY(done); QVERIFY(inspected.result.ok); QVERIFY(!inspected.allowed); QCOMPARE(inspected.generation, quint64(17)); QVERIFY(inspected.performedOn != gui);
        done = false; SshHostKeyReply stale;
        service.decide(request, 16, SshHostKeyDecision::TrustAndSave, [&](auto reply) { stale = std::move(reply); done = true; });
        QTRY_VERIFY(done); QVERIFY(!stale.allowed); QVERIFY(stale.conflict); QVERIFY(!QFile::exists(path));
        done = false; service.decide(request, 17, SshHostKeyDecision::ConnectOnce, [&](auto reply) { inspected = std::move(reply); done = true; });
        QTRY_VERIFY(done); QVERIFY(inspected.allowed); QCOMPARE(inspected.generation, quint64(17)); QVERIFY(!QFile::exists(path));
        done = false; service.decide(request, 17, SshHostKeyDecision::TrustAndSave, [&](auto reply) { stale = std::move(reply); done = true; });
        QTRY_VERIFY(done); QVERIFY(!stale.allowed); QVERIFY(stale.conflict);
        bool stopped = false; service.shutdown([&] { QCOMPARE(QThread::currentThreadId(), gui); stopped = true; }); QTRY_VERIFY(stopped);
    }
    void hostTrustCancelCannotAuthorizeOrUndoAnApprovedCommit() {
        for (bool afterCommit : {false, true}) {
            QTemporaryDir directory(fixtureDirectory()); const auto path = directory.filePath("ssh-host-keys.json");
            struct Block { std::atomic<bool> entered{false}, release{false}; };
            const auto block = std::make_shared<Block>();
            SshHostKeyService::Options options;
            options.commitGate = [block, afterCommit](const auto& commit) {
                const bool committed = afterCommit && commit(); block->entered = true;
                while (!block->release.load()) QThread::msleep(1);
                return afterCommit ? committed : commit();
            };
            SshHostKeyService service(path, options); const auto release = qScopeGuard([block] { block->release = true; });
            bool checked = false; const auto request = service.check(key(), 42, [&](auto reply) { QVERIFY(reply.result.ok); checked = true; }); QTRY_VERIFY(checked);
            SshHostKeyReply result; bool done = false;
            service.decide(request, 42, SshHostKeyDecision::TrustAndSave, [&](auto reply) { result = std::move(reply); done = true; });
            QTRY_VERIFY(block->entered.load()); service.cancel(request); block->release = true;
            QTRY_VERIFY(done); QVERIFY(result.canceled); QVERIFY(!result.allowed); QCOMPARE(result.committed, afterCommit); QCOMPARE(QFile::exists(path), afterCommit);
            if (afterCommit) { SshHostKeyStore reopened(path); QCOMPARE(reopened.inspect(key()).inspection->status, SshHostKeyStatus::Trusted); }
            bool stopped = false; service.shutdown([&] { stopped = true; }); QTRY_VERIFY(stopped);
        }
    }
    void hostTrustServiceRetiresWithoutBlockingGui() {
        QTemporaryDir directory(fixtureDirectory()); const auto path = directory.filePath("ssh-host-keys.json");
        const auto baseline = SshHostKeyService::activeWorkers();
        struct Block { std::atomic<bool> entered{false}, release{false}; };
        const auto block = std::make_shared<Block>(); SshHostKeyService::Options options;
        options.commitGate = [block](const auto& commit) { block->entered = true; while (!block->release.load()) QThread::msleep(1); return commit(); };
        auto service = std::make_unique<SshHostKeyService>(path, options); const auto release = qScopeGuard([block] { block->release = true; });
        bool checked = false; const auto request = service->check(key(), 1, [&](auto reply) { QVERIFY(reply.result.ok); checked = true; }); QTRY_VERIFY(checked);
        bool delivered = false; service->decide(request, 1, SshHostKeyDecision::TrustAndSave, [&](auto) { delivered = true; }); QTRY_VERIFY(block->entered.load());
        QElapsedTimer elapsed; elapsed.start(); service.reset(); QVERIFY(elapsed.elapsed() < 100);
        int heartbeat = 0; QTimer timer; connect(&timer, &QTimer::timeout, this, [&] { ++heartbeat; }); timer.start(1); QTRY_VERIFY(heartbeat >= 3);
        QVERIFY(!delivered); QVERIFY(!QFile::exists(path)); block->release = true; QTRY_COMPARE(SshHostKeyService::activeWorkers(), baseline);
        QVERIFY(!delivered); QVERIFY(!QFile::exists(path));
    }
    void malformedOrUnsafeTrustCacheFailsClosed() {
        QTemporaryDir directory(fixtureDirectory()); const auto path = directory.filePath("ssh-host-keys.json");
        for (const auto& invalid : {QByteArray("{}"), QByteArray("{\"schema\":2,\"records\":[]}"), QByteArray(1024 * 1024 + 1, 'x')}) {
            write(path, invalid); SshHostKeyStore store(path); const auto reply = store.inspect(key());
            QVERIFY(!reply.result.ok); QVERIFY(!reply.allowed); QVERIFY(!reply.inspection); QCOMPARE(contents(path), invalid);
        }
        SshHostKeyStore missingParent(directory.filePath("missing/ssh-host-keys.json")); QVERIFY(!missingParent.inspect(key()).result.ok);
#ifdef Q_OS_UNIX
        const auto link = directory.filePath("linked.json"); QVERIFY(QFile::link(path, link)); SshHostKeyStore unsafe(link); QVERIFY(!unsafe.inspect(key()).result.ok);
#endif
        QVERIFY_THROWS_EXCEPTION(std::exception, makeSshHostKey("owned.example", 22, QByteArray(16385, 'x')));
        auto forged = key(); forged.fingerprint = "SHA256:invented";
        SshHostKeyStore store(directory.filePath("clean.json")); QVERIFY(!store.inspect(forged).result.ok);
    }
};
QTEST_MAIN(SshHostKeysTest)
#include "ssh_host_keys.moc"
